"""PyTorch adapter — uses Ultralytics YOLO.

Accepts any format Ultralytics understands (.pt primarily, also .onnx/.engine
if torch backend can delegate; but prefer matching adapters per format).

Device hint mapping:
    cpu            → cpu
    cuda:N         → cuda:N  (NVIDIA)
    xpu[:N]        → xpu:N   (Intel Arc via IPEX — if available)
    mps            → mps     (Apple Silicon)

Ultralytics handles letterbox + NMS internally and returns normalized xyxy,
so we don't go through infer_utils here.
"""
from __future__ import annotations

import logging
import os
from typing import List, Optional

import numpy as np

import cv2

log = logging.getLogger("adapter_pytorch")


class PytorchAdapter:
    def __init__(self) -> None:
        self.model = None
        self.session = None  # truthy alias
        self.model_path: str = ""
        self.model_h: int = 0
        self.model_w: int = 0
        self.device: str = "cpu"
        # Layout/num_classes are not directly used — Ultralytics handles it.
        self.layout: str = "ultralytics"
        self.num_classes: int = 0

    def load(self, path: str, device_hint: str = "cpu") -> None:
        if not os.path.exists(path):
            raise FileNotFoundError(f"model not found: {path}")
        # Import lazily so the worker starts even if torch isn't installed.
        from ultralytics import YOLO  # type: ignore
        self.model = YOLO(path)
        self.session = self.model
        self.model_path = path
        self.device = device_hint or "cpu"
        # Probe imgsz from model if exposed (Ultralytics stores it in args).
        try:
            imgsz = int(self.model.overrides.get("imgsz", 640))
        except Exception:
            imgsz = 640
        self.model_h = imgsz
        self.model_w = imgsz
        log.info("loaded model=%s imgsz=%d device=%s",
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
        # Ultralytics expects BGR when given a numpy array (OpenCV convention).
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
