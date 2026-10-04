#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Fixtures for the single-access memory observer/verifier
(include/qemu/xemu-wasm-direct-mem-observe.h).

Pure bookkeeping tests: normalization, in-order consumption, every mismatch
class, missing/extra/cancelled/stale/overflow handling and a randomized
consume-and-mutate sweep, built with and without ASan/UBSan. Not integration
or replay proof: where the translator places the hooks is tested there.
"""

import argparse
import os
import re
import shlex
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HEADER = 'qemu/xemu-wasm-direct-mem-observe.h'

C = r'''
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include "qemu/xemu-wasm-direct-mem-observe.h"

#define OI(sz, sign, mmu) ((uint32_t)((((sz) | ((sign) ? 8 : 0)) << 5) | (mmu)))
#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
    exit(1); } } while (0)

static uint32_t next_inv = 1;
static int checks;

static uint64_t rng_state = 0x9e3779b97f4a7c15ull;
static uint64_t rnd(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

static void test_normalize(void)
{
    /* zero-extension */
    CHECK(xwmo_normalize(OI(0, 0, 1), 0x1234567890abcdefull) == 0xef);
    CHECK(xwmo_normalize(OI(1, 0, 1), 0x1234567890abcdefull) == 0xcdef);
    CHECK(xwmo_normalize(OI(2, 0, 1), 0x1234567890abcdefull) == 0x90abcdef);
    CHECK(xwmo_normalize(OI(3, 0, 1), 0x1234567890abcdefull) ==
          0x1234567890abcdefull);
    /* sign-extension */
    CHECK(xwmo_normalize(OI(0, 1, 1), 0x80) == 0xffffffffffffff80ull);
    CHECK(xwmo_normalize(OI(0, 1, 1), 0x7f) == 0x7f);
    CHECK(xwmo_normalize(OI(1, 1, 1), 0xffff8000) == 0xffffffffffff8000ull);
    CHECK(xwmo_normalize(OI(2, 1, 1), 0x80000000) == 0xffffffff80000000ull);
    CHECK(xwmo_normalize(OI(2, 1, 1), 0x7fffffff) == 0x7fffffff);
    /* a helper that already sign-extended agrees with one that did not */
    CHECK(xwmo_normalize(OI(0, 1, 1), 0xffffff80) ==
          xwmo_normalize(OI(0, 1, 1), 0x80));
    CHECK(xwmo_size_bits(OI(3, 0, 0)) == 64);
    checks += 11;
}

static void test_happy(void)
{
    static XwmoLog log;
    uint32_t inv = next_inv++;
    uint64_t v = 0;

    CHECK(xwmo_begin(&log, inv) == XWMO_OK);
    /* original: movsx eax, byte [0x1000]; mov [0x2000], eax */
    CHECK(xwmo_observe(&log, inv, 0x400000, 0x1000, OI(0, 1, 2),
                       XWMO_LOAD, 0xffffff80) == XWMO_OK);
    CHECK(xwmo_observe(&log, inv, 0x400003, 0x2000, OI(2, 0, 2),
                       XWMO_STORE, 0xffffff80) == XWMO_OK);
    CHECK(xwmo_shadow_load(&log, inv, 0x400000, 0x1000, OI(0, 1, 2), &v) ==
          XWMO_OK);
    CHECK(v == 0xffffffffffffff80ull);
    /* high bits beyond the 32-bit store are not compared */
    CHECK(xwmo_shadow_store(&log, inv, 0x400003, 0x2000, OI(2, 0, 2),
                            0xdeadbeefffffff80ull) == XWMO_OK);
    CHECK(xwmo_end(&log, inv) == XWMO_OK);
    /* an empty invocation passes */
    inv = next_inv++;
    CHECK(xwmo_begin(&log, inv) == XWMO_OK);
    CHECK(xwmo_end(&log, inv) == XWMO_OK);
    checks += 9;
}

/* one observed access, then a shadow access differing in one field */
static void one_mismatch(int field, XwmoResult want)
{
    static XwmoLog log;
    uint32_t inv = next_inv++;
    uint64_t v;
    XwmoKind kind = field == 0 ? XWMO_STORE : XWMO_LOAD;
    uint64_t pc = 0x1000 + (field == 1), addr = 0x2000 + (field == 2);
    uint32_t oi = field == 3 ? OI(1, 0, 3) : OI(2, 0, 3);
    XwmoResult r;

    CHECK(xwmo_begin(&log, inv) == XWMO_OK);
    if (field == 4) {
        CHECK(xwmo_observe(&log, inv, 0x1000, 0x2000, OI(1, 0, 3),
                           XWMO_STORE, 0x1234) == XWMO_OK);
        r = xwmo_shadow_store(&log, inv, 0x1000, 0x2000, OI(1, 0, 3),
                              0x1235);
    } else {
        CHECK(xwmo_observe(&log, inv, 0x1000, 0x2000, OI(2, 0, 3),
                           XWMO_LOAD, 7) == XWMO_OK);
        r = kind == XWMO_LOAD ?
            xwmo_shadow_load(&log, inv, pc, addr, oi, &v) :
            xwmo_shadow_store(&log, inv, pc, addr, oi, 7);
    }
    CHECK(r == want);
    CHECK(xwmo_end(&log, inv) == want);   /* first error is sticky */
    checks += 3;
}

static void test_mismatches(void)
{
    one_mismatch(0, XWMO_KIND);
    one_mismatch(1, XWMO_PC);
    one_mismatch(2, XWMO_ADDR);
    one_mismatch(3, XWMO_OI);
    one_mismatch(4, XWMO_VALUE);
}

static void test_missing_extra(void)
{
    static XwmoLog log;
    uint32_t inv = next_inv++;
    uint64_t v;

    CHECK(xwmo_begin(&log, inv) == XWMO_OK);
    CHECK(xwmo_shadow_load(&log, inv, 1, 2, OI(2, 0, 0), &v) == XWMO_MISSING);
    CHECK(xwmo_end(&log, inv) == XWMO_MISSING);

    inv = next_inv++;
    CHECK(xwmo_begin(&log, inv) == XWMO_OK);
    CHECK(xwmo_observe(&log, inv, 1, 2, OI(2, 0, 0), XWMO_LOAD, 5) == XWMO_OK);
    CHECK(xwmo_observe(&log, inv, 3, 4, OI(2, 0, 0), XWMO_LOAD, 6) == XWMO_OK);
    CHECK(xwmo_shadow_load(&log, inv, 1, 2, OI(2, 0, 0), &v) == XWMO_OK);
    CHECK(xwmo_end(&log, inv) == XWMO_EXTRA);
    checks += 8;
}

static void test_cancel(void)
{
    static XwmoLog log;
    uint32_t inv = next_inv++;
    uint64_t v;

    /* a fault after one access: nothing verifies, end is not a pass */
    CHECK(xwmo_begin(&log, inv) == XWMO_OK);
    CHECK(xwmo_observe(&log, inv, 1, 2, OI(2, 0, 0), XWMO_LOAD, 5) == XWMO_OK);
    xwmo_cancel(&log, inv);
    CHECK(xwmo_shadow_load(&log, inv, 1, 2, OI(2, 0, 0), &v) ==
          XWMO_UNVERIFIED);
    CHECK(xwmo_end(&log, inv) == XWMO_UNVERIFIED);

    /* a mismatch before the fault still reports the mismatch */
    inv = next_inv++;
    CHECK(xwmo_begin(&log, inv) == XWMO_OK);
    CHECK(xwmo_observe(&log, inv, 1, 2, OI(2, 0, 0), XWMO_LOAD, 5) == XWMO_OK);
    CHECK(xwmo_shadow_load(&log, inv, 9, 2, OI(2, 0, 0), &v) == XWMO_PC);
    xwmo_cancel(&log, inv);
    CHECK(xwmo_end(&log, inv) == XWMO_PC);

    /* cancelling another invocation has no effect */
    inv = next_inv++;
    CHECK(xwmo_begin(&log, inv) == XWMO_OK);
    xwmo_cancel(&log, inv - 1);
    CHECK(xwmo_end(&log, inv) == XWMO_OK);
    checks += 10;
}

static void test_stale(void)
{
    static XwmoLog log;
    uint32_t inv = next_inv++;
    uint64_t v;

    CHECK(xwmo_begin(&log, 0) == XWMO_STALE);
    CHECK(xwmo_begin(&log, inv) == XWMO_OK);
    CHECK(xwmo_observe(&log, inv + 1, 1, 2, OI(2, 0, 0), XWMO_LOAD, 5) ==
          XWMO_STALE);
    CHECK(xwmo_shadow_load(&log, inv - 1, 1, 2, OI(2, 0, 0), &v) ==
          XWMO_STALE);
    CHECK(xwmo_end(&log, inv + 1) == XWMO_STALE);
    CHECK(xwmo_end(&log, inv) == XWMO_OK);
    /* after end: no more observations or consumption */
    CHECK(xwmo_observe(&log, inv, 1, 2, OI(2, 0, 0), XWMO_LOAD, 5) ==
          XWMO_STALE);
    CHECK(xwmo_shadow_load(&log, inv, 1, 2, OI(2, 0, 0), &v) == XWMO_STALE);
    CHECK(xwmo_end(&log, inv) == XWMO_STALE);
    /* duplicate and older ids are rejected */
    CHECK(xwmo_begin(&log, inv) == XWMO_STALE);
    CHECK(xwmo_begin(&log, inv - 1) == XWMO_STALE);
    checks += 11;
}

static void test_overflow_bad_oi(void)
{
    static XwmoLog log;
    uint32_t inv = next_inv++;

    CHECK(xwmo_begin(&log, inv) == XWMO_OK);
    for (int i = 0; i < XWMO_MAX_ACCESSES; i++) {
        CHECK(xwmo_observe(&log, inv, i, i, OI(2, 0, 0), XWMO_LOAD, i) ==
              XWMO_OK);
    }
    CHECK(xwmo_observe(&log, inv, 0, 0, OI(2, 0, 0), XWMO_LOAD, 0) ==
          XWMO_OVERFLOW);
    CHECK(xwmo_end(&log, inv) == XWMO_OVERFLOW);

    inv = next_inv++;
    CHECK(xwmo_begin(&log, inv) == XWMO_OK);
    CHECK(xwmo_observe(&log, inv, 0, 0, (uint32_t)(4 << 5), XWMO_LOAD, 0) ==
          XWMO_BAD_OI);
    CHECK(xwmo_end(&log, inv) == XWMO_BAD_OI);
    checks += XWMO_MAX_ACCESSES + 5;
}

/* random access lists: consumed as observed -> OK; one field mutated -> the
 * matching error; the original's value consumed by loads round-trips */
static void test_random(int rounds)
{
    static XwmoLog log;

    for (int round = 0; round < rounds; round++) {
        XwmoAccess list[XWMO_MAX_ACCESSES];
        int n = rnd() % (XWMO_MAX_ACCESSES + 1);
        int mutate = rnd() % 3 == 0 && n ? (int)(rnd() % n) : -1;
        int field = rnd() % 5;
        uint32_t inv = next_inv++;
        XwmoResult want = XWMO_OK, got = XWMO_OK;

        for (int i = 0; i < n; i++) {
            list[i].pc = rnd();
            list[i].addr = rnd();
            list[i].oi = OI(rnd() % 4, rnd() & 1, rnd() % 32);
            list[i].kind = rnd() & 1 ? XWMO_STORE : XWMO_LOAD;
            list[i].value = rnd();
        }
        CHECK(xwmo_begin(&log, inv) == XWMO_OK);
        for (int i = 0; i < n; i++) {
            CHECK(xwmo_observe(&log, inv, list[i].pc, list[i].addr,
                               list[i].oi, list[i].kind, list[i].value) ==
                  XWMO_OK);
        }
        for (int i = 0; i < n; i++) {
            XwmoAccess a = list[i];
            XwmoResult r;
            uint64_t v;

            if (i == mutate) {
                switch (field) {
                case 0: a.kind = a.kind == XWMO_LOAD ? XWMO_STORE : XWMO_LOAD;
                        want = XWMO_KIND; break;
                case 1: a.pc ^= 1; want = XWMO_PC; break;
                case 2: a.addr ^= 4; want = XWMO_ADDR; break;
                case 3: a.oi ^= 1; want = XWMO_OI; break;
                default:
                    if (a.kind == XWMO_STORE) {
                        a.value ^= 1; want = XWMO_VALUE;
                    } else {
                        a.pc ^= 2; want = XWMO_PC;
                    }
                }
            }
            if (a.kind == XWMO_LOAD) {
                r = xwmo_shadow_load(&log, inv, a.pc, a.addr, a.oi, &v);
                if (r == XWMO_OK) {
                    CHECK(v == xwmo_normalize(a.oi, list[i].value));
                }
            } else {
                r = xwmo_shadow_store(&log, inv, a.pc, a.addr, a.oi,
                                      a.value);
            }
            if (got == XWMO_OK) {
                got = r;
            }
            checks++;
        }
        CHECK(got == want);
        CHECK(xwmo_end(&log, inv) == want);
        checks += 2;
    }
}

int main(int argc, char **argv)
{
    int rounds = argc > 1 ? atoi(argv[1]) : 20000;

    test_normalize();
    test_happy();
    test_mismatches();
    test_missing_extra();
    test_cancel();
    test_stale();
    test_overflow_bad_oi();
    test_random(rounds);
    printf("%d\n", checks);
    return 0;
}
'''


def qemu_encoding_matches():
    """The header's MemOp/MemOpIdx constants equal QEMU's own."""
    memop = (ROOT/'include/exec/memop.h').read_text()
    memopidx = (ROOT/'include/exec/memopidx.h').read_text()
    hdr = (ROOT/'include'/HEADER).read_text()
    size = int(re.search(r'MO_SIZE\s*=\s*(0x[0-9a-fA-F]+)', memop).group(1), 16)
    sign = int(re.search(r'MO_SIGN\s*=\s*(0x[0-9a-fA-F]+)', memop).group(1), 16)
    shift = int(re.search(r'return \(op << (\d+)\) \| idx;', memopidx).group(1))
    mine = {k: int(v, 0) for k, v in re.findall(
        r'#define (XWMO_(?:OI_MEMOP_SHIFT|MO_SIZE|MO_SIGN))\s+(\S+)', hdr)}
    assert mine == {'XWMO_OI_MEMOP_SHIFT': shift, 'XWMO_MO_SIZE': size,
                    'XWMO_MO_SIGN': sign}, mine


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--rounds', type=int, default=20000)
    args = parser.parse_args()
    qemu_encoding_matches()
    cc = shlex.split(os.environ.get('CC', 'cc'))
    flags = ['-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
             '-I' + str(ROOT/'include')]
    total = 0
    with tempfile.TemporaryDirectory(prefix='direct-mem-observe-') as tmp:
        tmp = Path(tmp)
        src = tmp/'test.c'
        src.write_text(C)
        for name, extra in [('plain', []),
                            ('sanitized', ['-fsanitize=address,undefined',
                                           '-fno-sanitize-recover=all', '-g'])]:
            exe = tmp/name
            subprocess.run([*cc, *flags, *extra, str(src), '-o', str(exe)],
                           check=True)
            out = subprocess.run([str(exe), str(args.rounds)], check=True,
                                 capture_output=True, text=True).stdout
            total += int(out.strip())
    print(f'PASS: {total} observer checks (plain + ASan/UBSan), '
          f'{args.rounds} random rounds each; MemOp encoding matches QEMU')


if __name__ == '__main__':
    main()
