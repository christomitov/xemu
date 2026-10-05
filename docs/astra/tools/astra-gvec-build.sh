#!/bin/bash
set -euo pipefail
root=/root/src/xemu-wasm
exec 9>"$root/.node-run.lock"
flock -n 9 || { echo EMULATOR_NODE_BUSY; exit 1; }
marker="$root/.integrator-building"
owner="ASTRA sat16 private build $$:$RANDOM"
(set -o noclobber; printf '%s\n' "$owner" > "$marker") || { echo BUILD_BUSY; exit 1; }
trap 'if grep -qxF "$owner" "$marker" 2>/dev/null; then rm -f "$marker"; fi' EXIT
source "$root/emsdk/emsdk_env.sh"
export PKG_CONFIG_PATH="$root/wasm-deps/target/lib/pkgconfig"
export EM_PKG_CONFIG_PATH="$PKG_CONFIG_PATH"
export CPATH="$root/wasm-deps/target/include"
export LDFLAGS="-L$root/wasm-deps/target/lib"
export BINARYEN_CORES=2 MALLOC_ARENA_MAX=2
cd "$root/xemu-astra/build"
python3 /tmp/astra-unit-command.py emmake ninja -j2
