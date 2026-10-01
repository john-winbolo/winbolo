#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Build WinBolo for WebAssembly using Emscripten
#
# Prerequisites:
#   - Install Emscripten SDK: https://emscripten.org/docs/getting_started/downloads.html
#   - Activate it: source /path/to/emsdk/emsdk_env.sh
#
# Usage:
#   cd winbolo-autopilot
#   ./winbolo/src/wasm/build.sh
#
# Output:
#   build-wasm-game/winbolo.html  - Open this in a browser
#   build-wasm-game/winbolo.js
#   build-wasm-game/winbolo.wasm
#   build-wasm-game/winbolo.data  - Preloaded assets
#
# Serve locally (threads need the COOP and COEP headers this script sends):
#   python3 src/wasm/serve_isolated.py build-wasm-game 8080
#   Then open http://localhost:8080/winbolo.html
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

# Check for emcmake
if ! command -v emcmake &> /dev/null; then
    echo "Error: emcmake not found. Please install and activate the Emscripten SDK."
    echo "  git clone https://github.com/emscripten-core/emsdk.git"
    echo "  cd emsdk && ./emsdk install latest && ./emsdk activate latest"
    echo "  source ./emsdk_env.sh"
    exit 1
fi

BUILD_DIR="$REPO_ROOT/build-wasm-game"

echo "=== Configuring WASM build ==="
emcmake cmake -B "$BUILD_DIR" -S "$SCRIPT_DIR" \
    -DCMAKE_BUILD_TYPE=Release

echo ""
echo "=== Building ==="
cmake --build "$BUILD_DIR" -j$(nproc 2>/dev/null || echo 4)

echo ""
echo "=== Build complete ==="
echo "Output files in: $BUILD_DIR/"
echo ""
echo "To test locally (threads need the COOP and COEP headers this script sends):"
echo "  python3 $SCRIPT_DIR/serve_isolated.py $BUILD_DIR 8080"
echo "  Open http://localhost:8080/winbolo.html"
