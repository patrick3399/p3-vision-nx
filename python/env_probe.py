#!/usr/bin/env python3
"""Env probe — emit a single-line JSON describing available runtimes.

Output contract (stdout, one object, no trailing text):
    {"runtimes": ["onnx","openvino"],
     "devices":  {"onnx":["cuda:0","cpu"], "openvino":["cpu","intel:gpu"]}}

stderr may contain human-readable import errors (caller ignores).
Exit code is 0 even when everything failed (result fields then empty).
"""
import json
import sys

runtimes = []
devices = {}

try:
    import onnxruntime as ort
    runtimes.append("onnx")
    provs = ort.get_available_providers()
    devs = []
    if "CUDAExecutionProvider" in provs:
        devs.append("cuda:0")
    if "OpenVINOExecutionProvider" in provs:
        devs.append("intel:gpu")
    devs.append("cpu")
    devices["onnx"] = devs
except Exception as e:
    print(f"[env_probe] onnxruntime unavailable: {e}", file=sys.stderr)

try:
    import openvino as ov
    runtimes.append("openvino")
    core = ov.Core()
    devs = []
    for d in core.available_devices:
        if d == "CPU":
            devs.append("cpu")
        elif d == "GPU":
            devs.append("intel:gpu")
        elif d == "NPU":
            devs.append("intel:npu")
        else:
            devs.append(d.lower())
    devices["openvino"] = devs or ["cpu"]
except Exception as e:
    print(f"[env_probe] openvino unavailable: {e}", file=sys.stderr)

try:
    import torch
    runtimes.append("pytorch")
    devs = ["cpu"]
    if torch.cuda.is_available():
        for i in range(torch.cuda.device_count()):
            devs.append(f"cuda:{i}")
    if hasattr(torch.backends, "mps") and torch.backends.mps.is_available():
        devs.append("mps")
    if hasattr(torch, "xpu") and torch.xpu.is_available():
        devs.append("xpu")
    devices["pytorch"] = devs
except Exception as e:
    print(f"[env_probe] torch unavailable: {e}", file=sys.stderr)

try:
    import tensorrt  # noqa: F401
    runtimes.append("tensorrt")
    devices["tensorrt"] = ["cuda:0"]
except Exception as e:
    print(f"[env_probe] tensorrt unavailable: {e}", file=sys.stderr)

json.dump({"runtimes": runtimes, "devices": devices}, sys.stdout)
sys.stdout.write("\n")
