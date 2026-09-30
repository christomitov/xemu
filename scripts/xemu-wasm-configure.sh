#!/usr/bin/env bash
# Configure an xemu wasm32 (emscripten) build in the current directory.
#
#   mkdir build-jit && cd build-jit
#   ../scripts/xemu-wasm-configure.sh --enable-tcg-wasm-jit   # wasm JIT
#   ../scripts/xemu-wasm-configure.sh                         # TCI only
#
# XEMU_WASM_ROOT points at the tree holding emsdk/ and wasm-deps/.
set -e
ROOT=${XEMU_WASM_ROOT:-/root/src/xemu-wasm}
SRC=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

source "$ROOT/emsdk/emsdk_env.sh" >/dev/null 2>&1
export PATH="$HOME/.local/bin:$PATH"
export PKG_CONFIG_PATH="$ROOT/wasm-deps/target/lib/pkgconfig"
export EM_PKG_CONFIG_PATH="$PKG_CONFIG_PATH"
export CPATH="$ROOT/wasm-deps/target/include"
export LDFLAGS="-L$ROOT/wasm-deps/target/lib"

exec emconfigure "$SRC/configure" --static --target-list=i386-softmmu \
    --cpu=wasm32 --cross-prefix= --disable-werror --enable-tcg-interpreter \
    --with-coroutine=wasm --without-default-features --enable-system \
    --disable-curl "$@" \
    --extra-cflags="-DXBOX=1 -O2 -matomics -mbulk-memory -DNDEBUG \
-DG_DISABLE_ASSERT -D_GNU_SOURCE -sASYNCIFY=1 -pthread -sPROXY_TO_PTHREAD=1 \
-sFORCE_FILESYSTEM -sALLOW_TABLE_GROWTH -sTOTAL_MEMORY=2300MB -sWASM_BIGINT \
-sMALLOC=mimalloc"
