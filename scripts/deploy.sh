#!/usr/bin/env bash
# Deploy p3-vision-nx to a Linux mediaserver host. Assumes scripts/build.sh
# has produced build/libp3_vision_nx.so.
#
# Run locally on the target machine with sudo privileges.
#
# Layout after deploy:
#   /opt/networkoptix/mediaserver/bin/plugins/p3_vision_nx/libp3_vision_nx.so
#   /opt/p3-vision-nx/python/*.py
#   /opt/p3-vision-nx/venv/              (created separately, see README)
#   /etc/systemd/system/p3-vision-nx-worker.service
#   /run/p3-vision-nx/                   (created by systemd RuntimeDirectory=)
#   /run/p3-vision-nx/worker_default.sock (bound by worker at startup)
#   /var/lib/p3-vision-nx/models/         (user-populated)

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${REPO_ROOT}/build}"

PLUGIN_DIR=/opt/networkoptix/mediaserver/bin/plugins/p3_vision_nx
PY_DIR=/opt/p3-vision-nx/python
UNIT_SRC="${REPO_ROOT}/systemd/p3-vision-nx-worker.service"
UNIT_DST=/etc/systemd/system/p3-vision-nx-worker.service
MEDIASERVER=networkoptix-mediaserver
WORKER=p3-vision-nx-worker.service
LEGACY_WORKER=nxworker.service   # transient unit from pre-v1.0 deploys

if [ ! -f "${BUILD_DIR}/libp3_vision_nx.so" ]; then
    echo "ERROR: ${BUILD_DIR}/libp3_vision_nx.so not found. Run scripts/build.sh first."
    exit 1
fi

if [ ! -f "${UNIT_SRC}" ]; then
    echo "ERROR: ${UNIT_SRC} not found."
    exit 1
fi

echo "=== Stop mediaserver ==="
sudo systemctl stop "${MEDIASERVER}" || true

echo "=== Stop worker (new + legacy transient) ==="
sudo systemctl stop "${WORKER}" 2>/dev/null || true
# Legacy transient unit from systemd-run based deployments. `stop` on a
# nonexistent unit is a no-op; suppress noise either way.
sudo systemctl stop "${LEGACY_WORKER}" 2>/dev/null || true

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

echo "=== Install systemd unit ==="
sudo install -m 0644 "${UNIT_SRC}" "${UNIT_DST}"
sudo systemctl daemon-reload
sudo systemctl enable "${WORKER}"

echo "=== Start worker ==="
# Start the worker first so the AF_UNIX socket exists before the
# mediaserver's DeviceAgent attempts its first connect.
sudo systemctl start "${WORKER}"
sleep 1
sudo systemctl is-active "${WORKER}"

echo "=== Start mediaserver ==="
sudo systemctl start "${MEDIASERVER}"
sleep 3
sudo systemctl is-active "${MEDIASERVER}"

echo ""
echo "Deployed. Check Desktop Client -> refresh plugin list."
echo "Worker logs: journalctl -u ${WORKER} -f"
