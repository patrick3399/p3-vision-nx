"""TensorRT adapter — Ultralytics wrapper for .engine files.

TensorRT needs NVIDIA GPU + matching CUDA/cuDNN/TRT on the host. On N100
(Intel-only) this adapter's backing deps don't install, so env_probe won't
expose it — this file still deploys so NVIDIA nodes have it ready.

Accepts only `.engine` paths. Device is always CUDA.
"""
from __future__ import annotations

import logging
import os
from typing import List, Optional

import numpy as np

import cv2

log = logging.getLogger("adapter_tensorrt")


class TensorrtAdapter:
    def __init__(self) -> None:
        self.model = None
        self.session = None
        self.model_path: str = ""
        self.model_h: int = 0
        self.model_w: int = 0
        self.device: str = "cuda:0"
        self.layout: str = "ultralytics"
        self.num_classes: int = 0

    def load(self, path: str, device_hint: str = "cuda:0") -> None:
        if not os.path.exists(path):
            raise FileNotFoundError(f"model not found: {path}")
        if not path.endswith(".engine"):
            log.warning("TensorrtAdapter.load path not .engine: %s", path)
        # Import lazily — requires torch + ultralytics + trt.
        from ultralytics import YOLO  # type: ignore
        self.model = YOLO(path, task="detect")
        self.session = self.model
        self.model_path = path
        self.device = device_hint or "cuda:0"
        try:
            imgsz = int(self.model.overrides.get("imgsz", 640))
        except Exception:
            imgsz = 640
        self.model_h = imgsz
        self.model_w = imgsz
        log.info("loaded engine=%s imgsz=%d device=%s",
                 path, imgsz, self.device)

    def infer(
        self,
        rgb_hwc: np.ndarray,
        conf_thr: float = 0.25,
        iou_thr: float = 0.45,
        class_filter: Optional[List[int]] = None,
    ) -> List[dict]:
        if self.model is None:
            return []
        bgr = cv2.cvtColor(rgb_hwc, cv2.COLOR_RGB2BGR)
        results = self.model.predict(
            bgr,
            conf=float(conf_thr),
            iou=float(iou_thr),
            classes=list(class_filter) if class_filter else None,
            device=self.device,
            verbose=False,
        )
        out: List[dict] = []
        if not results:
            return out
        boxes = results[0].boxes
        if boxes is None or boxes.shape[0] == 0:
            return out
        xyxyn = boxes.xyxyn.cpu().numpy()
        conf = boxes.conf.cpu().numpy()
        cls = boxes.cls.cpu().numpy()
        for i in range(xyxyn.shape[0]):
            x1, y1, x2, y2 = xyxyn[i]
            nx1 = max(0.0, min(1.0, float(x1)))
            ny1 = max(0.0, min(1.0, float(y1)))
            nx2 = max(0.0, min(1.0, float(x2)))
            ny2 = max(0.0, min(1.0, float(y2)))
            if nx2 <= nx1 or ny2 <= ny1:
                continue
            out.append({
                "xyxy": [nx1, ny1, nx2, ny2],
                "score": float(conf[i]),
                "cls": int(cls[i]),
            })
        return out
