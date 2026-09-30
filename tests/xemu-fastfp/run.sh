#!/bin/sh
# Build and run the fast_fp differential test natively (host gcc/clang).
#   tests/xemu-fastfp/run.sh [iterations-per-config] [seed]
# Needs glib-2.0 headers (for the osdep shim).  Builds in $OUT (default:
# a temp dir); does not touch any meson build directory.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/../.." && pwd)
OUT=${OUT:-$(mktemp -d)}
CC=${CC:-cc}
CFLAGS="-O2 -g -Wall -Wno-unused-function -I$HERE/shim -I$SRC/include -I$SRC/fpu $(pkg-config --cflags glib-2.0)"
nice -n 10 $CC $CFLAGS -c "$SRC/fpu/softfloat.c" -o "$OUT/softfloat.o"
nice -n 10 $CC $CFLAGS -c "$HERE/fastfp-test.c" -o "$OUT/fastfp-test.o"
$CC -o "$OUT/fastfp-test" "$OUT/fastfp-test.o" "$OUT/softfloat.o" -lm $(pkg-config --libs glib-2.0)
nice -n 10 "$OUT/fastfp-test" "$@"
