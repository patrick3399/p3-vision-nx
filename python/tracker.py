"""Minimal multi-object tracker (ByteTrack-lite).

Scope:
- Pure IoU-based greedy matching between incoming detections and existing
  tracks. No Kalman prediction, no ReID, no camera motion compensation.
- Per-instance state only. One `ByteTrackLite` instance per IPC connection
  (≡ per NX camera); no shared counter across cameras.
- `max_age`-frame grace period so a briefly-occluded object keeps its ID
  when it reappears.

Why not scipy.optimize.linear_sum_assignment (Hungarian)?
  Typical CCTV frame has fewer than ~20 detections and fewer than ~30 live
  tracks, so O(N*M) greedy matching runs in microseconds and we avoid a
  scipy dependency (the worker venv stays ~150 MB).

Class-match modes:
  "strict"         — detection only matches a track with identical COCO
                     class id. Safe, but car/truck/bus flicker at distance
                     breaks tracks.
  "vehicle_group"  — car/truck/bus are interchangeable, and
                     bicycle/motorcycle are interchangeable. All other
                     classes are strict. Default — balances safety with
                     COCO's known confusables.
  "any"            — class is ignored during matching. Risks cross-class
                     ID swaps when two different objects overlap; only use
                     on scenes dominated by a single class family.

Output contract:
  `update(boxes)` receives a list of dicts `{'xyxy':[x1,y1,x2,y2],
  'score':float, 'cls':int}` (xyxy in [0,1] frame-normalized) and returns
  the same list with a new `'track_id': int` field on each box. Never
  reorders, never drops. track_id is always >= 1 (monotonically increasing
  within this tracker's lifetime); value 0 is reserved for "no track" and
  will never be emitted.
"""
from __future__ import annotations

from typing import List, Optional

import numpy as np


# COCO-80 class groups treated as interchangeable when
# match_mode == "vehicle_group". Every group member maps to the same
# canonical key, so _class_match_ok() compares canonical keys instead of
# raw class ids.
#   2 car, 5 bus, 7 truck           — "4-wheel vehicle"
#   1 bicycle, 3 motorcycle         — "2-wheel vehicle"
# Everything else stays strict (class id == class id).
_VEHICLE_GROUP_4W = frozenset({2, 5, 7})
_VEHICLE_GROUP_2W = frozenset({1, 3})


def _canonical_cls(cls_id: int, mode: str) -> int:
    """Map a COCO class id to the canonical key used for matching.

    In "strict" / unknown modes, returns cls_id unchanged. In
    "vehicle_group", collapses the vehicle families to a single key per
    family. In "any", returns a constant — all detections compare equal.
    """
    if mode == "any":
        return -1
    if mode == "vehicle_group":
        if cls_id in _VEHICLE_GROUP_4W:
            return -100  # sentinel — not a valid COCO id
        if cls_id in _VEHICLE_GROUP_2W:
            return -101
    return cls_id


def _iou_matrix(a_xyxy: np.ndarray, b_xyxy: np.ndarray) -> np.ndarray:
    """Pairwise IoU between two sets of xyxy boxes.

    a_xyxy:  (N, 4)
    b_xyxy:  (M, 4)
    returns: (N, M)

    All coords in the same frame (doesn't matter if normalized or pixel —
    IoU is scale-invariant).
    """
    if a_xyxy.size == 0 or b_xyxy.size == 0:
        return np.zeros((a_xyxy.shape[0], b_xyxy.shape[0]), dtype=np.float32)

    # Broadcast to (N, M, 4).
    a = a_xyxy[:, None, :]
    b = b_xyxy[None, :, :]

    inter_x1 = np.maximum(a[..., 0], b[..., 0])
    inter_y1 = np.maximum(a[..., 1], b[..., 1])
    inter_x2 = np.minimum(a[..., 2], b[..., 2])
    inter_y2 = np.minimum(a[..., 3], b[..., 3])
    inter_w = np.clip(inter_x2 - inter_x1, 0.0, None)
    inter_h = np.clip(inter_y2 - inter_y1, 0.0, None)
    inter_area = inter_w * inter_h

    a_area = np.clip(a[..., 2] - a[..., 0], 0.0, None) \
           * np.clip(a[..., 3] - a[..., 1], 0.0, None)
    b_area = np.clip(b[..., 2] - b[..., 0], 0.0, None) \
           * np.clip(b[..., 3] - b[..., 1], 0.0, None)
    union = a_area + b_area - inter_area
    # Guard against zero-area boxes.
    union = np.where(union <= 0.0, 1e-9, union)
    return (inter_area / union).astype(np.float32)


class _Track:
    __slots__ = ("id", "xyxy", "cls", "time_since_update", "hits")

    def __init__(self, track_id: int, xyxy: np.ndarray, cls: int):
        self.id: int = track_id
        self.xyxy: np.ndarray = xyxy    # shape (4,), float32, [0,1]
        self.cls: int = cls
        self.time_since_update: int = 0
        self.hits: int = 1


class ByteTrackLite:
    """Greedy-IoU tracker, one instance per camera.

    Parameters:
        iou_thr:  minimum IoU to consider a detection-track pair a match.
                  0.3 is the ByteTrack default for high-confidence tracks
                  and works well for 30 fps CCTV (frame-to-frame motion
                  produces IoU > 0.5 for most pedestrian/vehicle speeds).
        max_age:  number of inference ticks a track is kept alive with no
                  matching detection. Default 90 — generous enough that a
                  brief occlusion (~3 s at 30 fps, longer when the plugin
                  samples at lower rates) doesn't split a track into two
                  events. Bump higher for slow-moving scenes; lower for
                  crowded ones where stale tracks would snap onto
                  unrelated detections.
        match_mode:  class-constraint policy — see module docstring.
                  "strict" | "vehicle_group" | "any". Default
                  "vehicle_group" because COCO's car/truck/bus flicker is
                  the most common cause of spurious track breaks.
    """

    def __init__(
        self,
        iou_thr: float = 0.3,
        max_age: int = 90,
        match_mode: str = "vehicle_group",
    ):
        self._next_id: int = 1
        self._tracks: List[_Track] = []
        self._iou_thr = float(iou_thr)
        self._max_age = int(max_age)
        self._match_mode = self._normalize_mode(match_mode)

    @staticmethod
    def _normalize_mode(m: str) -> str:
        m = (m or "").strip().lower()
        if m in ("strict", "vehicle_group", "any"):
            return m
        return "vehicle_group"

    def set_params(
        self,
        iou_thr: Optional[float] = None,
        max_age: Optional[int] = None,
        match_mode: Optional[str] = None,
    ) -> None:
        """Reconfigure without resetting live tracks.

        Called from worker.py when the DeviceAgent pushes a new config.
        Only the parameters provided are updated; existing tracks keep
        their IDs so a settings change mid-stream doesn't split events.
        """
        if iou_thr is not None:
            self._iou_thr = float(iou_thr)
        if max_age is not None:
            self._max_age = int(max_age)
        if match_mode is not None:
            self._match_mode = self._normalize_mode(match_mode)

    def live_count(self) -> int:
        """Number of tracks currently alive (including briefly-missed ones)."""
        return len(self._tracks)

    def update(self, boxes: List[dict]) -> List[dict]:
        """Assign a stable `track_id` to each box; age out stale tracks.

        Returns the same `boxes` list object (mutated in place with a new
        `track_id` field). Input ordering is preserved.
        """
        n_det = len(boxes)

        # Fast path: no detections at all — just age tracks and drop expired.
        if n_det == 0:
            self._age_and_prune()
            return boxes

        det_xyxy = np.asarray(
            [b["xyxy"] for b in boxes], dtype=np.float32
        ).reshape(n_det, 4)
        det_cls = np.asarray(
            [int(b.get("cls", 0)) for b in boxes], dtype=np.int32
        )

        # Build matching matrix.
        if not self._tracks:
            # All detections become new tracks.
            for i, b in enumerate(boxes):
                b["track_id"] = self._new_track(det_xyxy[i], int(det_cls[i]))
            return boxes

        trk_xyxy = np.stack([t.xyxy for t in self._tracks], axis=0)
        trk_cls = np.asarray([t.cls for t in self._tracks], dtype=np.int32)

        iou = _iou_matrix(det_xyxy, trk_xyxy)   # (n_det, n_trk)
        if self._match_mode != "any":
            # Disallow cross-class (or cross-group) matches by driving IoU
            # to 0 on mismatched pairs. vehicle_group collapses COCO's
            # confusable vehicle families to a single canonical key so
            # e.g. (car ↔ truck) on the same physical object don't break
            # the track.
            det_canon = np.asarray(
                [_canonical_cls(int(c), self._match_mode) for c in det_cls],
                dtype=np.int32,
            )
            trk_canon = np.asarray(
                [_canonical_cls(int(c), self._match_mode) for c in trk_cls],
                dtype=np.int32,
            )
            class_match = (det_canon[:, None] == trk_canon[None, :])
            iou = iou * class_match.astype(np.float32)

        det_matched = [False] * n_det
        trk_matched = [False] * len(self._tracks)
        det_to_track: List[Optional[int]] = [None] * n_det

        # Greedy: repeatedly pick the global maximum, lock it in, mask out
        # that row and column. Terminates when max drops below iou_thr.
        while True:
            idx = int(np.argmax(iou))
            best = float(iou.flat[idx])
            if best < self._iou_thr:
                break
            di, ti = divmod(idx, iou.shape[1])
            if det_matched[di] or trk_matched[ti]:
                # Shouldn't happen — we zeroed out matched rows/cols — but
                # be defensive against floating-point degeneracies.
                iou[di, ti] = -1.0
                continue
            det_matched[di] = True
            trk_matched[ti] = True
            det_to_track[di] = ti
            # Mask row + column so we don't pick them again.
            iou[di, :] = -1.0
            iou[:, ti] = -1.0

        # Apply matches: update track state + assign track_id to detection.
        for di, ti in enumerate(det_to_track):
            if ti is None:
                continue
            trk = self._tracks[ti]
            trk.xyxy = det_xyxy[di]
            trk.cls = int(det_cls[di])
            trk.time_since_update = 0
            trk.hits += 1
            boxes[di]["track_id"] = trk.id

        # Age unmatched *existing* tracks; prune those past max_age.
        #
        # ORDER MATTERS: this must run BEFORE we append new tracks below.
        # trk_matched was sized at entry (= original len(self._tracks));
        # if we created new tracks first they'd extend self._tracks past
        # trk_matched's length and this enumerate would raise IndexError on
        # trk_matched[ti]. Symptom of that bug was "infer failed: list index
        # out of range" the moment the first unmatched detection hit a
        # non-empty tracker.
        for ti, trk in enumerate(self._tracks):
            if not trk_matched[ti]:
                trk.time_since_update += 1
        self._tracks = [t for t in self._tracks
                        if t.time_since_update <= self._max_age]

        # Unmatched detections → new tracks (immediate ID assignment).
        # Done AFTER pruning so brand-new tracks aren't aged on the same
        # frame they're born.
        for di in range(n_det):
            if not det_matched[di]:
                boxes[di]["track_id"] = self._new_track(
                    det_xyxy[di], int(det_cls[di]))

        return boxes

    def _new_track(self, xyxy: np.ndarray, cls: int) -> int:
        tid = self._next_id
        self._next_id += 1
        self._tracks.append(_Track(tid, xyxy.copy(), cls))
        return tid

    def _age_and_prune(self) -> None:
        for t in self._tracks:
            t.time_since_update += 1
        self._tracks = [t for t in self._tracks
                        if t.time_since_update <= self._max_age]
