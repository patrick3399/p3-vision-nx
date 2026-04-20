"""ONNX adapter.

Uses the shared infer_utils decode path, so yolo26 (1,K,6) end-to-end and
yolov8 (1,4+C,N) raw outputs are both supported. Input shape is read
dynamically from the ONNX graph — supports 320 / 640 / 1280 letterbox
inputs interchangeably.

`device_hint` unifies the adapter signature across runtimes, but ONNX
Runtime providers are picked at session construction, so the hint only
controls CUDA vs CPU provider preference today. On the N100 target it will
always land on CPUExecutionProvider.
"""
from __future__ import annotations

import logging
from typing import List, Optional

import numpy as np
import onnxruntime as ort

from infer_utils import (
    LAYOUT_YOLO26,
    LAYOUT_YOLOV8,
    decode_boxes,
    detect_layout,
    preprocess_chw_f32,
)

log = logging.getLogger("adapter_onnx")


class OnnxAdapter:
    def __init__(self) -> None:
        self.session: Optional[ort.InferenceSession] = None
        self.input_name: str = ""
        self.model_h: int = 0
        self.model_w: int = 0
        self.model_path: str = ""
        self.num_classes: int = 0
        self.layout: str = LAYOUT_YOLOV8
        self.device: str = "cpu"

    def load(self, path: str, device_hint: str = "cpu") -> None:
        providers: List[str]
        hint = (device_hint or "cpu").strip().lower()
        if hint.startswith("cuda"):
            providers = ["CUDAExecutionProvider", "CPUExecutionProvider"]
        else:
            providers = ["CPUExecutionProvider"]
        sess = ort.InferenceSession(path, providers=providers)
        inp = sess.get_inputs()[0]
        shape = list(inp.shape)  # e.g. [1, 3, 640, 640]
        if len(shape) != 4:
            raise RuntimeError(f"unexpected input rank: {shape}")
        mh, mw = shape[2], shape[3]
        if not isinstance(mh, int) or not isinstance(mw, int):
            raise RuntimeError(f"dynamic input shape not supported yet: {shape}")
        self.session = sess
        self.input_name = inp.name
        self.model_h = int(mh)
        self.model_w = int(mw)
        self.model_path = path
        self.device = hint

        out_shape = list(sess.get_outputs()[0].shape)
        self.layout, self.num_classes = detect_layout(out_shape)
        log.info("loaded model=%s input=%dx%d layout=%s out_shape=%s providers=%s",
                 path, self.model_h, self.model_w, self.layout, out_shape,
                 sess.get_providers())

    def infer(
        self,
        rgb_hwc: np.ndarray,
        conf_thr: float = 0.25,
        iou_thr: float = 0.45,
        class_filter: Optional[List[int]] = None,
    ) -> List[dict]:
        if self.session is None:
            return []
        h0, w0 = rgb_hwc.shape[:2]
        tensor, ratio, (pad_x, pad_y) = preprocess_chw_f32(
            rgb_hwc, self.model_h, self.model_w)
        out = self.session.run(None, {self.input_name: tensor})[0]
        return decode_boxes(
            out, self.layout, conf_thr, iou_thr, class_filter,
            w0, h0, ratio, pad_x, pad_y)
