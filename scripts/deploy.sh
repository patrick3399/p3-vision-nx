#!/usr/bin/env bash
# Deploy p3-vision-nx to a Linux mediaserver host. Assumes scripts/build.sh
# has produced build/libp3_vision_nx.so.
#
# Run locally on the target machine with sudo privileges.
#
# Layout after deploy:
#   /opt/networkoptix/mediaserver/bin/plugins/p3_vision_nx/libp3_vision_nx.so
#   /opt/p3-vision-nx/python/*.py
#   /opt/p3-vision-nx/venv/              (created separately, see docs)
#   /run/p3-vision-nx/worker_default.sock (created at runtime by worker)
#   /var/lib/p3-vision-nx/models/         (user-populated)

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${REPO_ROOT}/build}"

PLUGIN_DIR=/opt/networkoptix/mediaserver/bin/plugins/p3_vision_nx
PY_DIR=/opt/p3-vision-nx/python
SERVICE=networkoptix-mediaserver

if [ ! -f "${BUILD_DIR}/libp3_vision_nx.so" ]; then
    echo "ERROR: ${BUILD_DIR}/libp3_vision_nx.so not found. Run scripts/build.sh first."
    exit 1
fi

echo "=== Stop mediaserver ==="
sudo systemctl stop "${SERVICE}"

echo "=== Install .so ==="
sudo mkdir -p "${PLUGIN_DIR}"
sudo cp "${BUILD_DIR}/libp3_vision_nx.so" "${PLUGIN_DIR}/libp3_vision_nx.so"
sudo chown networkoptix:networkoptix "${PLUGIN_DIR}/libp3_vision_nx.so"
sudo chmod 644 "${PLUGIN_DIR}/libp3_vision_nx.so"

echo "=== Install Python worker ==="
sudo mkdir -p "${PY_DIR}"
sudo cp "${REPO_ROOT}"/python/*.py "${PY_DIR}/"
sudo chown -R networkoptix:networkoptix "${PY_DIR}"
sudo chmod 644 "${PY_DIR}"/*.py

echo "=== Start mediaserver ==="
sudo systemctl start "${SERVICE}"
sleep 3
sudo systemctl is-active "${SERVICE}"

echo ""
echo "Deployed. Check Desktop Client -> refresh plugin list."
