# P3 Vision for Nx Witness

Self-hosted, multi-runtime computer-vision analytics plugin for Nx Witness
/ NxVMS 6.x. Drop-in alternative to NX AI Manager with the following trade-offs:

|                       | NX AI Manager (`nxai_plugin`)   | P3 Vision (this project)         |
| ---                   | ---                             | ---                              |
| Model distribution    | cloud upload / download          | local files, no cloud            |
| Runtime backends (default) | ONNX (CPU / OpenVINO / CUDA / JetPack) | ONNX + OpenVINO (AGPL-clean)     |
| Runtime backends (opt-in) | —                             | PyTorch/TensorRT/ROCm/CoreML/MLX *(AGPL-aware, planned)* |
| NMS                   | baked into ONNX graph            | optional — end-to-end YOLO26 supported |
| Model source          | NX cloud / convert tutorials     | user-exported ONNX (Ultralytics CLI on dev machine) |
| UX (ROI / mask / class filter) | —                        | aligned with NX AI Manager conventions |
| Pipeline stacking     | supported                        | planned                          |

License: MIT for all user-authored files. NX SDK-derived files (plugin.cpp,
engine.cpp, device_agent.cpp, coco80.*) retain their MPL-2.0 headers per
NX's sample license. See `LICENSE` and `NOTICE`.

---

## Repo layout

```
CMakeLists.txt                               build entry (Linux + Windows)
src/nx/vms_server_plugins/analytics/sample/  C++ plugin (6 cpp + 6 h)
python/                                      inference worker + adapters
scripts/build.sh                             configure + build wrapper
scripts/deploy.sh                            install to /opt + restart mediaserver
docs/nx-plugin-plan.md                       historical design doc (Phase 0–v1.0)
SDK_PIN.txt                                  pinned NX Metadata SDK version + URL
```

---

## Runtime layout (after `scripts/deploy.sh`)

| Path | Owner | Purpose |
| --- | --- | --- |
| `/opt/networkoptix/mediaserver/bin/plugins/p3_vision_nx/libp3_vision_nx.so` | `networkoptix` | plugin library loaded by mediaserver |
| `/opt/p3-vision-nx/python/*.py`     | `networkoptix` | worker + adapters |
| `/opt/p3-vision-nx/venv/`            | `networkoptix` | isolated Python environment (onnxruntime, openvino, ultralytics, etc.) |
| `/run/p3-vision-nx/worker_default.sock` | `networkoptix` | AF_UNIX socket between C++ DeviceAgent and Python worker |
| `/var/lib/p3-vision-nx/models/`     | `networkoptix` | user-supplied `.pt` / `.onnx` / `.engine` files |

---

## Build + deploy

Prerequisites on the target Linux host (Ubuntu 24.04):
```
sudo apt install build-essential cmake ninja-build pkg-config python3-venv
```

Download the NX Metadata SDK (version pinned in `SDK_PIN.txt`) and unpack it
somewhere outside the repo. Point CMake at it:

```
SDK_DIR=/path/to/unpacked/metadata_sdk scripts/build.sh
scripts/deploy.sh
```

Confirm in Desktop Client: the plugin appears as **P3 Vision** v1.0.0 by
**Dev By Patrick3399**. Click the refresh button if it still shows cached
manifest from a previous build.

---

## Python venv setup (one-time)

Default install — AGPL-clean, ONNX + OpenVINO only:

```
sudo python3 -m venv /opt/p3-vision-nx/venv
sudo /opt/p3-vision-nx/venv/bin/pip install --upgrade pip
sudo /opt/p3-vision-nx/venv/bin/pip install \
    numpy opencv-python-headless onnxruntime openvino
sudo chown -R networkoptix:networkoptix /opt/p3-vision-nx/venv
```

TensorRT / ROCm / CoreML / MLX extras install only on matching hardware.
`python/env_probe.py` auto-detects which backends are usable and exposes
them in the Engine's runtime picker.

**Do NOT `pip install ultralytics` on the mediaserver** unless you
explicitly want the entire plugin deployment to fall under AGPL-3.0.
See `NOTICE` section 4 and `python/optional/adapter_pytorch.py`.

---

## Exporting YOLO weights (run on your dev machine, NOT the mediaserver)

The plugin consumes `.onnx` files from `/var/lib/p3-vision-nx/models/`.
Produce them on your own development workstation:

```
# On a separate dev machine, in a throwaway venv:
python3 -m venv /tmp/yolo-export
source /tmp/yolo-export/bin/activate
pip install ultralytics
yolo export model=yolo26n.pt format=onnx imgsz=640 opset=15 simplify=true

# Copy the produced yolo26n.onnx to the mediaserver:
scp yolo26n.onnx user@mediaserver:/tmp/
ssh user@mediaserver "sudo install -o networkoptix -g networkoptix -m 644 \
    /tmp/yolo26n.onnx /var/lib/p3-vision-nx/models/"
```

This mirrors the pattern in Network Optix's own conversion tutorials:
https://github.com/scailable/nxai-model-to-onnx . Ultralytics AGPL-3.0
obligations attach to your dev machine where the export runs; the exported
ONNX weights are model output and do not carry AGPL into the plugin
runtime on the mediaserver.

---

## Development notes

- **Source-of-truth** is this repo. Previous iterations on the N100 target
  (`~/nx_plugin/`) and the Windows staging (`phase1-staging/`) are both
  superseded by this tree.
- NX Metadata SDK is **not vendored** — see `SDK_PIN.txt`.
- Historical design doc at `docs/nx-plugin-plan.md` traces the v0.1–v1.0
  milestones; paths referenced there (`nx_custom_plugin`, `nx_plugin`)
  belong to pre-v1.0 deployments and do not reflect the current layout.
