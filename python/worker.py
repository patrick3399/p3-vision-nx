#!/usr/bin/env python3
"""Multi-runtime inference worker.

Binds /run/p3-vision-nx/worker_default.sock, accepts multiple C++ clients,
and for every `type=frame` message runs inference on the raw RGB body.

The adapter (onnx / openvino / pytorch / tensorrt) is chosen by the
`runtime` field in each `config` message; changing runtime at any time
triggers a new adapter instantiation + model reload.

Wire format:
    Control msg  : <uint32 BE length> <utf8 JSON payload>
    Frame msg    : <uint32 BE length> <utf8 JSON header>
                   <body_bytes of size = h * stride>          (no framing)

Per-connection state = one client = one camera = one adapter.
"""
from __future__ import annotations

import importlib
import json
import logging
import os
import socket
import struct
import sys
import threading
import time
from typing import List, Optional

import numpy as np

# Plugin deploys adapters alongside worker.py.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from infer_utils import (  # noqa: E402  — needs sys.path patch above
    filter_boxes_by_regions,
    names_to_ids,
    normalize_to_pixel_polygon,
)
from tracker import ByteTrackLite  # noqa: E402

SOCKET_PATH = "/run/p3-vision-nx/worker_default.sock"

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(threadName)s] %(message)s",
    stream=sys.stderr,
)
log = logging.getLogger("worker")


# Adapter factory table: module name + class name.
# Imported lazily so a missing optional dep (torch, openvino, tensorrt) only
# breaks the runtime that needs it, not the whole worker.
#
# License boundary:
#   onnx / openvino / tensorrt adapters are permissively licensed (plugin
#   stays MIT-clean).
#   `pytorch` imports ultralytics (AGPL-3.0). The default deploy does NOT
#   ship python/optional/adapter_pytorch.py — selecting the "pytorch"
#   runtime in the Engine will fail with ImportError unless the user
#   opts in to AGPL. See NOTICE section 4.
_ADAPTER_TABLE = {
    "onnx":      ("adapter_onnx",     "OnnxAdapter"),
    "openvino":  ("adapter_openvino", "OpenvinoAdapter"),
    "pytorch":   ("adapter_pytorch",  "PytorchAdapter"),  # AGPL — optional
    "tensorrt":  ("adapter_tensorrt", "TensorrtAdapter"),
}


def make_adapter(runtime: str):
    """Instantiate an adapter by runtime name. Raises on unknown / import fail."""
    key = (runtime or "onnx").strip().lower()
    if key not in _ADAPTER_TABLE:
        raise ValueError(f"unknown runtime: {runtime!r}")
    mod_name, cls_name = _ADAPTER_TABLE[key]
    mod = importlib.import_module(mod_name)
    cls = getattr(mod, cls_name)
    return cls()


def recv_exact(sock: socket.socket, n: int) -> bytes:
    chunks = []
    remaining = n
    while remaining > 0:
        chunk = sock.recv(remaining)
        if not chunk:
            return b""
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


def send_msg(sock: socket.socket, obj: dict) -> None:
    data = json.dumps(obj, separators=(",", ":")).encode("utf-8")
    sock.sendall(struct.pack(">I", len(data)) + data)


def decode_rgb_body(body: bytes, w: int, h: int, stride: int) -> np.ndarray:
    arr = np.frombuffer(body, dtype=np.uint8)
    if stride == w * 3:
        return arr.reshape(h, w, 3)
    # Padded stride — reshape per-row then crop.
    full = arr.reshape(h, stride)
    return full[:, : w * 3].reshape(h, w, 3)


# Minimum crop dimension (pixels). A degenerate ROI (e.g. a single-click
# BoxFigure with width/height < 8 px) would letterbox to near-empty and wreck
# model recall — safer to fall back to full-frame than to pretend the user
# drew something useful. 8 px is well below any realistic monitoring ROI.
_MIN_CROP_PX = 8


def _crop_to_roi(
    img: np.ndarray,
    roi_norm: Optional[List[List[float]]],
    w: int, h: int,
):
    """Extract ROI bounding rectangle and crop the frame for inference.

    ROI is a real pixel crop applied *before* `adapter.infer()`, so the
    model literally never sees pixels outside the ROI. This aligns with
    NX AI Manager's official convention (see nx.docs.scailable.net /
    input-masks-and-roi: "parts outside of the region will not be seen
    by the model").

    The caller is expected to remap adapter-emitted bbox coords (which are
    normalized to [0,1] of whatever image was passed in) back to the
    original frame — see the frame handler in serve().

    roi_norm is a list of [x, y] points in 0-1 coords; we take its
    axis-aligned bounding rectangle. This is robust to point ordering —
    `device_agent.cpp::parseBoxFigure` currently emits 4 CCW corners but
    min/max works regardless, and would still do the right thing if the
    C++ side ever changed the encoding.

    Returns (crop_img, crop_x1_px, crop_y1_px, crop_w_px, crop_h_px).
    When ROI is absent / degenerate / would crop to < _MIN_CROP_PX,
    returns the original img with offset=(0,0) and size=(w,h) — the
    remap becomes an identity transform so caller code stays branch-free.
    """
    if not roi_norm or len(roi_norm) < 2:
        return img, 0, 0, w, h

    xs: List[float] = []
    ys: List[float] = []
    for p in roi_norm:
        if isinstance(p, list) and len(p) >= 2:
            try:
                xs.append(float(p[0]))
                ys.append(float(p[1]))
            except Exception:
                continue
    if len(xs) < 2 or len(ys) < 2:
        return img, 0, 0, w, h

    x1 = max(0, min(w, int(min(xs) * w)))
    y1 = max(0, min(h, int(min(ys) * h)))
    x2 = max(0, min(w, int(max(xs) * w)))
    y2 = max(0, min(h, int(max(ys) * h)))
    cw = x2 - x1
    ch = y2 - y1
    if cw < _MIN_CROP_PX or ch < _MIN_CROP_PX:
        return img, 0, 0, w, h
    return img[y1:y2, x1:x2], x1, y1, cw, ch


def serve(conn: socket.socket, peer: str) -> None:
    log.info("client connected peer=%s", peer)
    # State per-connection (one camera).
    adapter = None
    current_runtime = ""
    current_device = ""
    current_model = ""
    conf_thr = 0.30
    iou_thr = 0.45
    # Class filter tri-state (see infer_utils.names_to_ids):
    #   None  — setting absent, emit all classes
    #   []    — user explicitly unchecked everything, skip inference entirely
    #   [ids] — emit only these COCO ids
    class_filter_ids: Optional[List[int]] = None
    # Region polygons arrive as [[x,y],...] in 0-1 coords from the DeviceAgent.
    # Semantics:
    #   roi_norm       — axis-aligned box corners (C++ emits 4 CCW corners from
    #                    BoxFigure); taken as *pixel crop* applied BEFORE
    #                    inference — the model never sees pixels outside this.
    #   inclusive_norm — polygon; post-filter by bbox anchor (kept only if
    #                    center is inside at least one inclusive polygon).
    #   exclusive_norm — polygon; post-filter (dropped if inside).
    # poly_px_cache caches pixel-space masks for Incl/Excl lazily, keyed by
    # (w,h), so resolution changes mid-stream rebuild automatically. Cleared
    # on every config message.
    roi_norm: Optional[List[List[float]]] = None
    inclusive_norm: Optional[List[List[float]]] = None
    exclusive_norm: Optional[List[List[float]]] = None
    poly_px_cache: dict = {}  # (w,h) → (incl_px, excl_px); ROI not in cache
    # Per-camera tracker. Instantiated lazily on the first config message;
    # the serve() scope guarantees per-connection isolation so track_id
    # spaces never collide between cameras.
    tracker: Optional[ByteTrackLite] = None
    cam = "?"
    frames_seen = 0
    frames_infer = 0
    frames_region_dropped = 0
    try:
        while True:
            hdr = recv_exact(conn, 4)
            if len(hdr) < 4:
                break
            (length,) = struct.unpack(">I", hdr)
            if length <= 0 or length > 64 * 1024 * 1024:
                log.warning("bogus length=%d, dropping client", length)
                break
            payload = recv_exact(conn, length)
            if len(payload) < length:
                break
            try:
                msg = json.loads(payload.decode("utf-8"))
            except Exception as exc:
                log.warning("bad JSON: %s", exc)
                continue

            mtype = msg.get("type")
            cam = msg.get("camera_uuid", cam)

            if mtype == "attach":
                log.info("attach cam=%s", cam)

            elif mtype == "detach":
                log.info("detach cam=%s frames_seen=%d frames_infer=%d "
                         "region_dropped=%d",
                         cam, frames_seen, frames_infer, frames_region_dropped)
                break

            elif mtype == "config":
                new_runtime = (msg.get("runtime") or "onnx").strip().lower()
                new_device = (msg.get("device") or "cpu").strip().lower()
                new_model = msg.get("model_path") or ""
                conf_thr = float(msg.get("conf", conf_thr))
                iou_thr = float(msg.get("iou", iou_thr))

                # `classes` is a list of COCO-80 names the user ticked.
                # Missing key → no filter, empty list → filter to nothing.
                classes_val = msg.get("classes", None)
                if classes_val is None:
                    class_filter_ids = None
                elif isinstance(classes_val, list):
                    class_filter_ids = names_to_ids([str(c) for c in classes_val])
                else:
                    log.warning("classes not a list: %r", classes_val)
                    class_filter_ids = None

                # Polygons. Each key is either absent (no polygon), a list of
                # [x,y] pairs in 0-1 coords, or anything else which we coerce
                # to "no polygon".
                def _coerce_poly(v):
                    if not isinstance(v, list) or len(v) < 3:
                        return None
                    pts = []
                    for p in v:
                        if isinstance(p, list) and len(p) >= 2:
                            try:
                                pts.append([float(p[0]), float(p[1])])
                            except Exception:
                                continue
                    return pts if len(pts) >= 3 else None

                roi_norm = _coerce_poly(msg.get("roi"))
                inclusive_norm = _coerce_poly(msg.get("inclusive_mask"))
                exclusive_norm = _coerce_poly(msg.get("exclusive_mask"))
                # Blow away the pixel cache — any change here invalidates it.
                poly_px_cache = {}

                # Tracker config (optional keys — defaults applied on first
                # config if absent). track_max_age is in inference ticks,
                # not seconds; track_class_match ∈ {strict, vehicle_group,
                # any}. See tracker.py for semantics.
                track_max_age = msg.get("track_max_age", None)
                track_class_match = msg.get("track_class_match", None)

                # Lazy-init the per-camera tracker on first config. Config
                # changes mid-stream (model swap, threshold change) don't
                # reset the tracker — IoU continuity should survive parameter
                # tweaks; only a client disconnect resets track IDs.
                if tracker is None:
                    tracker = ByteTrackLite(
                        iou_thr=0.3,
                        max_age=int(track_max_age) if track_max_age is not None else 90,
                        match_mode=str(track_class_match) if track_class_match is not None else "vehicle_group",
                    )
                else:
                    # Existing tracker — push updated params without
                    # wiping live track state (keeps IDs stable across
                    # user Apply clicks).
                    tracker.set_params(
                        max_age=int(track_max_age) if track_max_age is not None else None,
                        match_mode=str(track_class_match) if track_class_match is not None else None,
                    )

                log.info("config cam=%s runtime=%s device=%s model=%s "
                         "conf=%.2f iou=%.2f classes=%s "
                         "roi=%s incl=%s excl=%s "
                         "trk_max_age=%s trk_match=%s",
                         cam, new_runtime, new_device, new_model,
                         conf_thr, iou_thr,
                         ("all" if class_filter_ids is None
                          else (class_filter_ids if class_filter_ids
                                else "none")),
                         (len(roi_norm) if roi_norm else 0),
                         (len(inclusive_norm) if inclusive_norm else 0),
                         (len(exclusive_norm) if exclusive_norm else 0),
                         track_max_age, track_class_match)

                # Swap adapter if runtime changed.
                if new_runtime != current_runtime or adapter is None:
                    try:
                        adapter = make_adapter(new_runtime)
                        current_runtime = new_runtime
                        current_device = ""
                        current_model = ""
                        log.info("adapter_swap cam=%s -> %s",
                                 cam, new_runtime)
                    except Exception as exc:
                        log.error("adapter instantiate failed runtime=%s: %s",
                                  new_runtime, exc)
                        adapter = None
                        current_runtime = ""

                # (Re)load model if path or device changed and adapter is ready.
                if adapter is not None and new_model:
                    need_load = (
                        new_model != current_model
                        or new_device != current_device
                        or getattr(adapter, "session", None) is None
                    )
                    if need_load:
                        try:
                            adapter.load(new_model, device_hint=new_device)
                            current_model = new_model
                            current_device = new_device
                        except Exception as exc:
                            log.error("model load failed runtime=%s dev=%s "
                                      "path=%s: %s",
                                      current_runtime, new_device, new_model, exc)

            elif mtype == "frame":
                w = int(msg.get("w", 0))
                h = int(msg.get("h", 0))
                stride = int(msg.get("stride", w * 3))
                fid = int(msg.get("id", 0))
                body_size = h * stride
                if body_size <= 0 or body_size > 128 * 1024 * 1024:
                    log.warning("bad frame dims w=%d h=%d stride=%d", w, h, stride)
                    break
                body = recv_exact(conn, body_size)
                if len(body) < body_size:
                    log.warning("short body on cam=%s", cam)
                    break
                frames_seen += 1
                boxes: list = []
                infer_ms = 0.0
                # Set to True once the per-camera tracker has been ticked for
                # this frame. Lets the shared post-block below age the tracker
                # on skipped/failed frames without double-ticking on the
                # success path.
                tracker_ticked = False
                short_circuit = (class_filter_ids is not None
                                 and len(class_filter_ids) == 0)
                if short_circuit:
                    # User unchecked every class → don't even run the model.
                    frames_infer += 1
                    if frames_infer <= 3 or frames_infer % 300 == 0:
                        log.info("infer cam=%s rt=%s dev=%s id=%d "
                                 "SKIPPED (empty class filter)",
                                 cam, current_runtime, current_device, fid)
                elif adapter is not None and getattr(adapter, "session", None) is not None:
                    try:
                        img = decode_rgb_body(body, w, h, stride)

                        # ROI is a real pixel crop applied *before* inference.
                        # Inclusive/Exclusive masks stay post-filter (they're
                        # coordinate-level, not pixel-level). Crop offset +
                        # size are needed to remap adapter bbox coords (which
                        # are normalized to whatever image it received) back
                        # to original-frame coords. When no ROI → identity.
                        crop, crop_x1, crop_y1, cw, ch = _crop_to_roi(
                            img, roi_norm, w, h)
                        was_cropped = (cw != w or ch != h
                                       or crop_x1 != 0 or crop_y1 != 0)

                        t0 = time.perf_counter()
                        # class_filter_ids is None → adapter emits all classes;
                        # non-empty list → adapter filters internally.
                        all_boxes = adapter.infer(
                            crop,
                            conf_thr=conf_thr,
                            iou_thr=iou_thr,
                            class_filter=class_filter_ids,
                        )
                        infer_ms = (time.perf_counter() - t0) * 1000.0

                        # Remap crop-normalized xyxy → original-frame normalized.
                        # Adapter already divides by its input's (w0,h0) in
                        # decode_boxes, so xyxy is [0,1] of the crop. We undo
                        # that and rewrite to [0,1] of the original frame.
                        if was_cropped:
                            inv_w = 1.0 / w
                            inv_h = 1.0 / h
                            for b in all_boxes:
                                try:
                                    x1n, y1n, x2n, y2n = b["xyxy"]
                                except Exception:
                                    continue
                                b["xyxy"] = [
                                    (float(x1n) * cw + crop_x1) * inv_w,
                                    (float(y1n) * ch + crop_y1) * inv_h,
                                    (float(x2n) * cw + crop_x1) * inv_w,
                                    (float(y2n) * ch + crop_y1) * inv_h,
                                ]

                        # Post-filter only does Inclusive/Exclusive masks now.
                        # ROI has already done its job at the crop stage —
                        # passing roi_px=None keeps filter logic untouched for
                        # back-compat.
                        n_before = len(all_boxes)
                        if inclusive_norm or exclusive_norm:
                            key = (w, h)
                            poly_px = poly_px_cache.get(key)
                            if poly_px is None:
                                poly_px = (
                                    normalize_to_pixel_polygon(inclusive_norm, w, h),
                                    normalize_to_pixel_polygon(exclusive_norm, w, h),
                                )
                                poly_px_cache[key] = poly_px
                            boxes = filter_boxes_by_regions(
                                all_boxes, w, h,
                                None,           # ROI no longer participates
                                poly_px[0], poly_px[1])
                        else:
                            boxes = all_boxes
                        n_dropped = n_before - len(boxes)
                        frames_region_dropped += n_dropped

                        # Stamp each box with its stable track_id. Must run
                        # AFTER all filters (class / ROI crop / incl / excl)
                        # — the tracker never sees a detection the user has
                        # rejected, so a briefly-ticked-off class resumes the
                        # same track_id when re-enabled within max_age.
                        # tracker is instantiated in the config handler; can
                        # still be None here if config hasn't arrived yet (be
                        # defensive).
                        if tracker is not None:
                            boxes = tracker.update(boxes)
                            tracker_ticked = True
                        tracks_live = tracker.live_count() if tracker else 0

                        frames_infer += 1
                        if frames_infer <= 3 or frames_infer % 300 == 0 or n_dropped:
                            top_dbg = sorted(all_boxes,
                                             key=lambda b: -b.get("score", 0))[:3]
                            crop_desc = (
                                "%d,%d,%dx%d" % (crop_x1, crop_y1, cw, ch)
                                if was_cropped else "full"
                            )
                            log.info("infer cam=%s rt=%s dev=%s id=%d "
                                     "boxes=%d/%d infer_ms=%.1f "
                                     "(%dx%d stride=%d) filter=%s "
                                     "crop=%s region=%s region_drop=%d "
                                     "tracks=%d top3=%s",
                                     cam, current_runtime, current_device,
                                     fid, len(boxes), n_before, infer_ms,
                                     w, h, stride,
                                     ("all" if class_filter_ids is None
                                      else class_filter_ids),
                                     crop_desc,
                                     ("%d/%d" % (
                                         len(inclusive_norm) if inclusive_norm else 0,
                                         len(exclusive_norm) if exclusive_norm else 0)),
                                     n_dropped, tracks_live, top_dbg)
                    except Exception as exc:
                        # log.exception (not log.error) so the traceback
                        # lands in the journal — cheap insurance when a
                        # pipeline stage (tracker, crop, adapter swap)
                        # raises unexpectedly. A bare "infer failed: list
                        # index out of range" with no traceback is a much
                        # more expensive thing to debug.
                        log.exception("infer failed: %s", exc)

                # On any frame where we didn't run inference (empty class
                # filter, adapter not yet ready, inference raised) still age
                # the tracker so max_age advances and stale tracks eventually
                # prune. boxes is [] in these paths → update() just ticks
                # time_since_update on every existing track and drops anything
                # past max_age.
                if not tracker_ticked and tracker is not None:
                    tracker.update(boxes)

                reply = {
                    "type": "result",
                    "camera_uuid": cam,
                    "id": fid,
                    "boxes": boxes,
                    "infer_ms": round(infer_ms, 2),
                    "runtime": current_runtime,
                    "device": current_device,
                }
                send_msg(conn, reply)

            else:
                log.info("unknown type=%r cam=%s", mtype, cam)
    except (ConnectionResetError, BrokenPipeError, OSError) as exc:
        log.info("client io error cam=%s: %s", cam, exc)
    except Exception as exc:
        log.exception("serve crashed cam=%s: %s", cam, exc)
    finally:
        log.info("client disconnected peer=%s cam=%s frames_seen=%d frames_infer=%d "
                 "region_dropped=%d",
                 peer, cam, frames_seen, frames_infer, frames_region_dropped)
        try:
            conn.close()
        except Exception:
            pass


def main() -> None:
    os.makedirs(os.path.dirname(SOCKET_PATH), exist_ok=True)
    try:
        os.unlink(SOCKET_PATH)
    except FileNotFoundError:
        pass

    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.bind(SOCKET_PATH)
    os.chmod(SOCKET_PATH, 0o666)
    sock.listen(16)
    log.info("listening on %s (pid=%d)", SOCKET_PATH, os.getpid())

    try:
        while True:
            conn, _ = sock.accept()
            peer = f"fd={conn.fileno()}"
            t = threading.Thread(target=serve, args=(conn, peer), daemon=True)
            t.start()
    except KeyboardInterrupt:
        log.info("shutdown")
    finally:
        try:
            sock.close()
        except Exception:
            pass
        try:
            os.unlink(SOCKET_PATH)
        except Exception:
            pass


if __name__ == "__main__":
    main()
