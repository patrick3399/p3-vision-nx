"""OpenVINO adapter.

Accepts:
  * .onnx       — read directly via ov.Core().read_model()
  * .xml (+bin) — native IR

Device hint mapping:
    cpu        → CPU
    intel:gpu  → GPU (first iGPU / dGPU, e.g. Intel UHD on N100)
    intel:npu  → NPU (Meteor Lake / Arrow Lake)
    auto       → AUTO (openvino picks)

Output decoding is shared with OnnxAdapter via infer_utils — same yolo26 /
yolov8 auto-detect.
"""
from __future__ import annotations

import logging
import os
from typing import List, Optional

import numpy as np

import openvino as ov  # type: ignore

from infer_utils import (
    LAYOUT_YOLO26,
    LAYOUT_YOLOV8,
    decode_boxes,
    detect_layout,
    preprocess_chw_f32,
)

log = logging.getLogger("adapter_openvino")


_DEVICE_MAP = {
    "cpu": "CPU",
    "intel:gpu": "GPU",
    "intel:npu": "NPU",
    "auto": "AUTO",
}


def map_device(hint: str) -> str:
    hint = (hint or "").strip().lower()
    if hint in _DEVICE_MAP:
        return _DEVICE_MAP[hint]
    # Pass-through for things like "GPU.0" / "GPU.1" / explicit OV names.
    return hint.upper() if hint else "AUTO"


class OpenvinoAdapter:
    def __init__(self) -> None:
        self.core: Optional[ov.Core] = None
        self.compiled = None
        self.session = None  # truthy alias for worker's is-loaded check
        self.input_port = None
        self.output_port = None
        self.model_h: int = 0
        self.model_w: int = 0
        self.model_path: str = ""
        self.device: str = ""
        self.num_classes: int = 0
        self.layout: str = LAYOUT_YOLOV8

    def load(self, path: str, device_hint: str = "cpu") -> None:
        if not os.path.exists(path):
            raise FileNotFoundError(f"model not found: {path}")
        if self.core is None:
            self.core = ov.Core()
        model = self.core.read_model(path)

        # Resolve input spatial dims from the graph; OpenVINO sometimes has
        # dynamic dims so fall back to 640x640 if not static.
        inp = model.input(0)
        shape = list(inp.get_partial_shape())
        # partial_shape dims may be dynamic — coerce conservatively.
        def _to_int(d, fallback):
            try:
                v = d.get_length() if hasattr(d, "get_length") else int(d)
                return int(v) if v and v > 0 else fallback
            except Exception:
                return fallback

        if len(shape) == 4:
            mh = _to_int(shape[2], 640)
            mw = _to_int(shape[3], 640)
        else:
            mh, mw = 640, 640

        dev = map_device(device_hint)
        try:
            compiled = self.core.compile_model(model, device_name=dev)
        except Exception as exc:
            log.warning("compile on %s failed (%s) — retrying AUTO", dev, exc)
            compiled = self.core.compile_model(model, device_name="AUTO")
            dev = "AUTO"

        self.compiled = compiled
        self.session = compiled  # truthy
        self.input_port = compiled.input(0)
        self.output_port = compiled.output(0)
        self.model_h = mh
        self.model_w = mw
        self.model_path = path
        self.device = dev

        out_shape = list(self.output_port.get_partial_shape())
        # Coerce to plain ints where possible for layout detect.
        coerced = []
        for d in out_shape:
            try:
                v = d.get_length() if hasattr(d, "get_length") else int(d)
                coerced.append(int(v) if v and v > 0 else None)
            except Exception:
                coerced.append(None)
        self.layout, self.num_classes = detect_layout(coerced)

        log.info("loaded model=%s input=%dx%d device=%s layout=%s out_shape=%s",
                 path, self.model_h, self.model_w, self.device, self.layout, coerced)

    def infer(
        self,
        rgb_hwc: np.ndarray,
        conf_thr: float = 0.25,
        iou_thr: float = 0.45,
        class_filter: Optional[List[int]] = None,
    ) -> List[dict]:
        if self.compiled is None:
            return []
        h0, w0 = rgb_hwc.shape[:2]
        tensor, ratio, (pad_x, pad_y) = preprocess_chw_f32(
            rgb_hwc, self.model_h, self.model_w)
        result = self.compiled([tensor])
        out = result[self.output_port]
        out = np.asarray(out)
        return decode_boxes(
            out, self.layout, conf_thr, iou_thr, class_filter,
            w0, h0, ratio, pad_x, pad_y)
