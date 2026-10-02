#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Test all 765 direct-call signatures and their rejection domain.

Run with a host compiler: python3 tests/unit/test-tci-direct-call.py
Or actual Wasm/Asyncify (CC defaults to emcc with --wasm):
    python3 tests/unit/test-tci-direct-call.py --wasm
The latter suspends inside every typed helper, testing argument/return replay.
"""

import argparse
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--wasm", action="store_true")
    wasm = parser.parse_args().wasm
    generated = subprocess.check_output(
        [sys.executable, str(ROOT / "scripts/gen-tci-direct-call.py")])
    assert generated == (ROOT / "tcg/tci-direct-call.c.inc").read_bytes()
    code = ["""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif
static uint64_t observed;
static unsigned calls;
static uint64_t mix(uint64_t h, uint64_t v)
{
    return ((h << 7) | (h >> 57)) ^ v;
}
""", generated.decode()]
    signatures = []
    for kind, rtype in enumerate(["void", "uint32_t", "uint64_t"]):
        for nargs in range(8):
            for mask in range(1 << nargs):
                sig = kind | (nargs << 2) | (mask << 5)
                types = ["uint64_t" if mask & (1 << j) else "uint32_t"
                         for j in range(nargs)]
                params = ", ".join(f"{t} p{j}" for j, t in enumerate(types))
                code.append(f"static {rtype} h{sig}({params or 'void'}) {{\n")
                code.append(f"uint64_t h = {sig}; calls++;\n")
                code.append("""
#ifdef __EMSCRIPTEN__
    emscripten_sleep(0);
#endif
""")
                for j in range(nargs):
                    code.append(f"h = mix(h, p{j});\n")
                code.append("observed = h;\n")
                if kind:
                    code.append(f"return ({rtype})h;\n")
                code.append("}\n")
                signatures.append(sig)
    code.append("""
static const struct { unsigned sig; void *f; } cases[] = {
""")
    for sig in signatures:
        code.append(f"{{ {sig}, (void *)h{sig} }},\n")
    code.append("""
};
int main(void)
{
    uint64_t a[7], rv, want;
    const uint64_t sentinel = UINT64_C(0xabcdef13579b2468);
    for (unsigned trial = 0; trial < 3; trial++) {
        for (unsigned j = 0; j < 7; j++) {
            a[j] = trial == 0 ? UINT64_MAX - j :
                   trial == 1 ? UINT64_C(0x87654321fedcba98) * (j + 1) : j;
        }
        for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            unsigned sig = cases[i].sig, nargs = (sig >> 2) & 7;
            unsigned mask = sig >> 5, kind = sig & 3;
            want = sig;
            rv = sentinel;
            calls = 0;
            for (unsigned j = 0; j < nargs; j++) {
                want = mix(want, mask & (1u << j) ? a[j] : (uint32_t)a[j]);
            }
            assert(tci_direct_call(sig, cases[i].f, a, &rv));
            assert(calls == 1 && observed == want);
            assert(rv == (kind == 0 ? sentinel :
                          kind == 1 ? (uint32_t)want : want));
        }
    }
    for (unsigned sig = 0; sig < 4096; sig++) {
        unsigned nargs = (sig >> 2) & 7;
        bool valid = (sig & 3) < 3 && (sig >> 5) < (1u << nargs);
        if (!valid) {
            rv = sentinel;
            assert(!tci_direct_call(sig, NULL, NULL, &rv));
            assert(rv == sentinel);
        }
    }
    assert(!tci_direct_call(UINT32_MAX, NULL, NULL, NULL));
    assert(!tci_direct_call(4096, NULL, NULL, NULL));
    puts("PASS: 765 signatures x 3 patterns; "
         "invalid signatures have no effects");
    return 0;
}
""")
    with tempfile.TemporaryDirectory(prefix="test-tci-direct-") as tmp:
        src = Path(tmp) / "test.c"
        exe = Path(tmp) / ("test.js" if wasm else "test")
        src.write_text("".join(code))
        flags = (["-O2", "-sASYNCIFY=1", "-sENVIRONMENT=node",
                  "-sEXIT_RUNTIME=1", "-sASSERTIONS=1"] if wasm else ["-O0"])
        compiler = os.environ.get("CC", "emcc" if wasm else "cc")
        subprocess.run([*shlex.split(compiler), "-std=c11", "-Wall", "-Wextra",
                        *flags, str(src), "-o", str(exe)], check=True)
        runner = shlex.split(os.environ.get("NODE", "node")) if wasm else []
        subprocess.run([*runner, str(exe)], check=True, timeout=60)


if __name__ == "__main__":
    main()
