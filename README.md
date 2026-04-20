# P3 Vision for Nx Witness

Self-hosted, multi-runtime computer-vision analytics plugin for Nx Witness
/ NxVMS 6.x. Drop-in alternative to NX AI Manager with the following trade-offs:

|                       | NX AI Manager (`nxai_plugin`)   | P3 Vision (this project)         |
| ---                   | ---                             | ---                              |
| Model distribution    | cloud upload / download          | local files, no cloud            |
| Runtime backends      | ONNX (CPU / OpenVINO / CUDA / JetPack) | ONNX, PyTorch (CPU/CUDA/MPS/XPU), TensorRT, OpenVINO, ROCm, CoreML, MLX *(planned)* |
| NMS                   | baked into ONNX graph            | optional — end-to-end YOLO26 supported |
| Ecosystem alignment   | NX-internal                      | Ultralytics YOLO                 |
| UX (ROI / mask / class filter) | —                        | aligned with NX AI Manager conventions |
| Pipeline stacking     | supported                        | planned                          |

License: MPL-2.0. See `LICENSE`.

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

```
sudo python3 -m venv /opt/p3-vision-nx/venv
sudo /opt/p3-vision-nx/venv/bin/pip install --upgrade pip
sudo /opt/p3-vision-nx/venv/bin/pip install \
    numpy opencv-python-headless onnxruntime openvino ultralytics
sudo chown -R networkoptix:networkoptix /opt/p3-vision-nx/venv
```

TensorRT / ROCm / CoreML / MLX extras install only on matching hardware.
`python/env_probe.py` auto-detects which backends are usable and exposes
them in the Engine's runtime picker.

---

## Development notes

- **Source-of-truth** is this repo. Previous iterations on the N100 target
  (`~/nx_plugin/`) and the Windows staging (`phase1-staging/`) are both
  superseded by this tree.
- NX Metadata SDK is **not vendored** — see `SDK_PIN.txt`.
- Historical design doc at `docs/nx-plugin-plan.md` traces the v0.1–v1.0
  milestones; paths referenced there (`nx_custom_plugin`, `nx_plugin`)
  belong to pre-v1.0 deployments and do not reflect the current layout.
