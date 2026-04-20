"""Shared preprocess / postprocess for YOLO adapters (ONNX / OpenVINO / TRT).

Pytorch path (Ultralytics) does its own preprocess + decode, so it does NOT use
this module — only the raw-tensor adapters do.

Layout auto-detection:
  * (1, K, 6)        → yolo26 end-to-end:  [x1,y1,x2,y2,score,cls] (NMS baked in)
  * (1, 4+C, N)      → yolov8 raw:          cxcywh + raw class scores (needs NMS)
"""
from __future__ import annotations

from typing import List, Optional, Tuple

import cv2
import numpy as np

LAYOUT_YOLO26 = "yolo26"
LAYOUT_YOLOV8 = "yolov8"


# Ultralytics COCO-80 class names, index-aligned with standard pretrained YOLO
# checkpoints (yolov5/v8/v10/26). Used to translate the user's class-name
# selections (from the DeviceAgent CheckBoxGroup) into integer ids that the
# model actually outputs.
COCO80_NAMES: List[str] = [
    "person", "bicycle", "car", "motorcycle", "airplane", "bus", "train",
    "truck", "boat", "traffic light", "fire hydrant", "stop sign",
    "parking meter", "bench", "bird", "cat", "dog", "horse", "sheep", "cow",
    "elephant", "bear", "zebra", "giraffe", "backpack", "umbrella", "handbag",
    "tie", "suitcase", "frisbee", "skis", "snowboard", "sports ball", "kite",
    "baseball bat", "baseball glove", "skateboard", "surfboard",
    "tennis racket", "bottle", "wine glass", "cup", "fork", "knife", "spoon",
    "bowl", "banana", "apple", "sandwich", "orange", "broccoli", "carrot",
    "hot dog", "pizza", "donut", "cake", "chair", "couch", "potted plant",
    "bed", "dining table", "toilet", "tv", "laptop", "mouse", "remote",
    "keyboard", "cell phone", "microwave", "oven", "toaster", "sink",
    "refrigerator", "book", "clock", "vase", "scissors", "teddy bear",
    "hair drier", "toothbrush",
]

_COCO_NAME_TO_ID = {n: i for i, n in enumerate(COCO80_NAMES)}


# ---------------------------------------------------------------------------
# Region helpers.
#
# Polygons arrive from the Desktop Client as lists of [x,y] with coordinates
# normalized to 0-1 (origin top-left). The DeviceAgent (C++) parses the
# NX-wrapper ({"figure":{"points":[...]}}) and sends us plain point arrays via
# IPC, so this module never has to deal with the stringified-JSON form.
#
# Semantics (aligned with NX AI Manager conventions):
#     ROI            — pixel crop applied BEFORE inference (done in worker.py,
#                      not here). Passed through this module only as its
#                      axis-aligned bounding rect; not fed into
#                      filter_boxes_by_regions.
#     InclusiveMask  — polygon, post-filter: keep bbox if its center is inside.
#     ExclusiveMask  — polygon, post-filter: drop bbox if its center is inside.
#
#     valid_detection = inside(Inclusive or everywhere) AND NOT inside(Exclusive)
#
#   - Neither Inclusive nor Exclusive defined  →  all boxes pass
#   - A polygon with < 3 points is ignored (can't form an area)
#
# We keep the polygon in normalized form and only convert to a pixel-space
# int32 np.array lazily the first time a frame with a given (w, h) arrives,
# so that cameras changing resolution mid-stream don't crash us. The pixel
# cache is invalidated by the worker whenever a new config message arrives.
# ---------------------------------------------------------------------------


def normalize_to_pixel_polygon(
    pts_norm: Optional[List[List[float]]],
    w: int, h: int,
) -> Optional[np.ndarray]:
    """[[x,y],...] in 0-1 → int32 Nx2 pixel coords (for cv2.pointPolygonTest).

    Returns None when the input is empty / has fewer than 3 points (can't
    form a closed area).
    """
    if not pts_norm or len(pts_norm) < 3:
        return None
    try:
        arr = np.asarray(pts_norm, dtype=np.float32)
    except Exception:
        return None
    if arr.ndim != 2 or arr.shape[1] < 2:
        return None
    # Clamp to [0, 1] — Desktop Client sometimes lets the user drag slightly
    # out-of-bounds while editing; float noise near the edge shouldn't break
    # the cv2 test.
    arr[:, 0] = np.clip(arr[:, 0], 0.0, 1.0) * w
    arr[:, 1] = np.clip(arr[:, 1], 0.0, 1.0) * h
    return arr.astype(np.int32)


def _point_in(poly_int32: Optional[np.ndarray], x: float, y: float) -> bool:
    if poly_int32 is None:
        return False
    return cv2.pointPolygonTest(poly_int32, (float(x), float(y)), False) >= 0


def filter_boxes_by_regions(
    boxes: List[dict],
    w: int, h: int,
    roi_px: Optional[np.ndarray],
    inclusive_px: Optional[np.ndarray],
    exclusive_px: Optional[np.ndarray],
) -> List[dict]:
    """Drop boxes whose center is not in (ROI ∪ Inclusive) \\ Exclusive.

    If neither ROI nor Inclusive is defined, the positive-region test is
    skipped (full frame counts as allowed). ExclusiveMask is always
    subtractive when defined.
    """
    has_positive = roi_px is not None or inclusive_px is not None
    if not has_positive and exclusive_px is None:
        return boxes

    kept: List[dict] = []
    for b in boxes:
        try:
            x1, y1, x2, y2 = b["xyxy"]
        except Exception:
            continue
        # Centers in pixel coords (xyxy from adapter is already normalized 0-1).
        cx = (float(x1) + float(x2)) * 0.5 * w
        cy = (float(y1) + float(y2)) * 0.5 * h

        if has_positive:
            inside_pos = (_point_in(roi_px, cx, cy)
                          or _point_in(inclusive_px, cx, cy))
            if not inside_pos:
                continue
        if exclusive_px is not None and _point_in(exclusive_px, cx, cy):
            continue
        kept.append(b)
    return kept


def names_to_ids(names: Optional[List[str]]) -> Optional[List[int]]:
    """Translate a user-facing class-name list to COCO-80 indices.

    Returns:
      * None  — caller did not set any filter (setting missing) → "emit all"
      * []    — caller explicitly unchecked everything → "emit nothing"
                (worker short-circuits before running the model)
      * [ids] — checked names, unknown names silently dropped
    """
    if names is None:
        return None
    ids: List[int] = []
    for n in names:
        key = str(n).strip().lower()
        if key in _COCO_NAME_TO_ID:
            ids.append(_COCO_NAME_TO_ID[key])
    # Explicit empty stays empty (≠ None) so the worker can distinguish
    # "no filter" from "filter to nothing".
    return ids


def letterbox(
    img: np.ndarray,
    new_shape: Tuple[int, int],  # (model_h, model_w)
    color: Tuple[int, int, int] = (114, 114, 114),
) -> Tuple[np.ndarray, float, Tuple[int, int]]:
    h, w = img.shape[:2]
    mh, mw = new_shape
    r = min(mh / h, mw / w)
    new_h, new_w = int(round(h * r)), int(round(w * r))
    if (new_w, new_h) != (w, h):
        resized = cv2.resize(img, (new_w, new_h), interpolation=cv2.INTER_LINEAR)
    else:
        resized = img
    canvas = np.full((mh, mw, 3), color, dtype=np.uint8)
    pad_y = (mh - new_h) // 2
    pad_x = (mw - new_w) // 2
    canvas[pad_y:pad_y + new_h, pad_x:pad_x + new_w] = resized
    return canvas, r, (pad_x, pad_y)


def detect_layout(out_shape) -> Tuple[str, int]:
    """Given the ONNX/OV output tensor static shape, guess yolo26 vs yolov8."""
    shape = list(out_shape)
    if len(shape) == 3 and shape[-1] == 6:
        return LAYOUT_YOLO26, 0
    if len(shape) == 3 and isinstance(shape[1], int):
        return LAYOUT_YOLOV8, max(0, shape[1] - 4)
    return LAYOUT_YOLOV8, 0


def preprocess_chw_f32(
    rgb_hwc: np.ndarray,
    model_h: int,
    model_w: int,
) -> Tuple[np.ndarray, float, Tuple[int, int]]:
    """Letterbox → CHW float32 [0,1] with batch dim. Returns (tensor, ratio, pads)."""
    lb, ratio, pads = letterbox(rgb_hwc, (model_h, model_w))
    tensor = lb.astype(np.float32, copy=False) * (1.0 / 255.0)
    tensor = np.ascontiguousarray(tensor.transpose(2, 0, 1)[None, ...])
    return tensor, ratio, pads


def decode_boxes(
    out: np.ndarray,
    layout: str,
    conf_thr: float,
    iou_thr: float,
    class_filter: Optional[List[int]],
    w0: int, h0: int,
    ratio: float,
    pad_x: int, pad_y: int,
) -> List[dict]:
    """Decode raw model output to list of {'xyxy','score','cls'} in 0-1 frame coords."""
    if layout == LAYOUT_YOLO26:
        rows = out[0]
        if rows.ndim != 2 or rows.shape[1] != 6:
            return []
        mask = rows[:, 4] >= conf_thr
        if class_filter:
            keep_set = set(int(c) for c in class_filter)
            cls_mask = np.fromiter((int(c) in keep_set for c in rows[:, 5]),
                                    dtype=bool, count=rows.shape[0])
            mask = mask & cls_mask
        if not np.any(mask):
            return []
        sel = rows[mask]
        xyxy = sel[:, :4]
        cls_scores = sel[:, 4]
        cls_ids = sel[:, 5]
        idxs = list(range(len(sel)))  # end-to-end head already did NMS
    else:
        pred = out[0].T
        if pred.shape[1] < 5:
            return []
        boxes_cxcywh = pred[:, :4]
        scores_all = pred[:, 4:]
        cls_ids_all = scores_all.argmax(axis=1)
        cls_scores_all = scores_all.max(axis=1)
        mask = cls_scores_all >= conf_thr
        if class_filter:
            keep_set = set(int(c) for c in class_filter)
            cls_mask = np.fromiter((int(c) in keep_set for c in cls_ids_all),
                                    dtype=bool, count=len(cls_ids_all))
            mask = mask & cls_mask
        if not np.any(mask):
            return []
        boxes_cxcywh = boxes_cxcywh[mask]
        cls_scores = cls_scores_all[mask]
        cls_ids = cls_ids_all[mask]
        xyxy = np.empty_like(boxes_cxcywh)
        xyxy[:, 0] = boxes_cxcywh[:, 0] - boxes_cxcywh[:, 2] * 0.5
        xyxy[:, 1] = boxes_cxcywh[:, 1] - boxes_cxcywh[:, 3] * 0.5
        xyxy[:, 2] = boxes_cxcywh[:, 0] + boxes_cxcywh[:, 2] * 0.5
        xyxy[:, 3] = boxes_cxcywh[:, 1] + boxes_cxcywh[:, 3] * 0.5
        xywh = np.column_stack([
            xyxy[:, 0], xyxy[:, 1],
            xyxy[:, 2] - xyxy[:, 0],
            xyxy[:, 3] - xyxy[:, 1],
        ]).astype(np.float32)
        idxs = cv2.dnn.NMSBoxes(
            xywh.tolist(), cls_scores.astype(np.float32).tolist(),
            float(conf_thr), float(iou_thr))
        if len(idxs) == 0:
            return []
        if isinstance(idxs, np.ndarray):
            idxs = idxs.flatten().tolist()
        else:
            idxs = [int(i) for i in np.array(idxs).flatten()]

    out_list: List[dict] = []
    for i in idxs:
        x1, y1, x2, y2 = xyxy[i]
        x1 = (x1 - pad_x) / ratio
        y1 = (y1 - pad_y) / ratio
        x2 = (x2 - pad_x) / ratio
        y2 = (y2 - pad_y) / ratio
        nx1 = max(0.0, min(1.0, float(x1) / w0))
        ny1 = max(0.0, min(1.0, float(y1) / h0))
        nx2 = max(0.0, min(1.0, float(x2) / w0))
        ny2 = max(0.0, min(1.0, float(y2) / h0))
        if nx2 <= nx1 or ny2 <= ny1:
            continue
        out_list.append({
            "xyxy": [nx1, ny1, nx2, ny2],
            "score": float(cls_scores[i]),
            "cls": int(cls_ids[i]),
        })
    return out_list
