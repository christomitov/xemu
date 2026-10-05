#!/bin/bash
set -euo pipefail
root=/root/src/xemu-wasm
exec 9>"$root/.node-run.lock"
flock -n 9 || { echo EMULATOR_NODE_BUSY; exit 1; }
marker="$root/.integrator-building"
owner="ASTRA gvec sat16 fixtures $$:$RANDOM"
(set -o noclobber; printf '%s\n' "$owner" > "$marker") || { echo BUILD_BUSY; exit 1; }
trap 'if grep -qxF "$owner" "$marker" 2>/dev/null; then rm -f "$marker"; fi' EXIT
cd "$root/xemu-astra"
export MALLOC_ARENA_MAX=2 BINARYEN_CORES=2 PYTHONDONTWRITEBYTECODE=1
export CLANG_WASM="$root/emsdk/upstream/bin/clang"
export WASM_OPT="$root/emsdk/upstream/bin/wasm-opt"
python3 /tmp/astra-unit-command.py python3 tests/unit/test-wasm-gvec-sat16.py
CC='cc -fsanitize=address,undefined -fno-omit-frame-pointer' python3 /tmp/astra-unit-command.py python3 tests/unit/test-wasm-gvec-sat16.py
python3 /tmp/astra-unit-command.py python3 tests/unit/test-wasm-phase.py
python3 /tmp/astra-unit-command.py python3 tests/unit/test-wasm-notdirty-emitter.py --wasm
