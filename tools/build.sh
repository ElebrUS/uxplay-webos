#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
python3 tools/bootstrap.py
tools/build-deps.sh
cmake -S . -B build -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/.cache/arm-webos-linux-gnueabi_sdk-buildroot/share/buildroot/toolchainfile.cmake" \
  -DBUILD_RECEIVER=ON -DWEBOS14=ON -DWEBOS14_LGNC=ON -DBUILD_DIRECT_EXPERIMENT=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 6
python3 tools/package.py
