#!/usr/bin/env bash
# Build p3-vision-nx on Linux. Run on the target machine (or any matching
# Ubuntu 24.04 host) — cross-compile is not supported.
#
# Usage:
#   scripts/build.sh                          # uses defaults below
#   SDK_DIR=/path/to/sdk scripts/build.sh     # override SDK location

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${REPO_ROOT}/build}"
SDK_DIR="${SDK_DIR:-${HOME}/nx_plugin/third_party/metadata_sdk}"

if [ ! -d "${SDK_DIR}/src/nx/sdk" ]; then
    echo "ERROR: Metadata SDK not found at ${SDK_DIR}"
    echo "See SDK_PIN.txt for download URL + version."
    exit 1
fi

echo "=== Configure ==="
cmake -S "${REPO_ROOT}" -B "${BUILD_DIR}" \
      -DmetadataSdkDir="${SDK_DIR}" \
      -DCMAKE_BUILD_TYPE=Release

echo ""
echo "=== Build ==="
cmake --build "${BUILD_DIR}" -j

echo ""
echo "=== Output ==="
ls -lh "${BUILD_DIR}/libp3_vision_nx.so"
