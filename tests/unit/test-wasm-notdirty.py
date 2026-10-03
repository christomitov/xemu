#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Test the production code-protected precheck across dirty-client states."""

import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
TLB = (ROOT / "accel/tcg/cputlb.c").read_text()
PHYS = (ROOT / "system/physmem.c").read_text()


def function(source, name):
    match = re.search(r"(?m)^(?:static )?bool " + name + r"\([^;]*?\)\n\{",
                      source)
    assert match, name
    pos = match.end()
    depth = 1
    while depth:
        depth += (source[pos] == "{") - (source[pos] == "}")
        pos += 1
    return source[match.start():pos]


condition = re.search(
    r"if \((!notdirty_skip_tlb_cleanup.*?)\) \{\s*"
    r"trace_memory_notdirty_set_dirty", TLB, re.S)
assert condition
C = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define XBOX 1
#define EMSCRIPTEN 1
typedef uint64_t ram_addr_t;
enum { DIRTY_MEMORY_NV2A, DIRTY_MEMORY_NV2A_TEX, DIRTY_MEMORY_VGA,
       DIRTY_MEMORY_CODE, DIRTY_MEMORY_MIGRATION, DIRTY_MEMORY_NUM };
#define ALL ((1u << DIRTY_MEMORY_NUM) - 1)
#define CODE (1u << DIRTY_MEMORY_CODE)
static unsigned bits, calls, flips, flip_to;
static struct { uint64_t n_notdirty_fast_skip; } xemu_wasm_stats;
#define XSTAT_INC(field) (xemu_wasm_stats.field++)
static bool physical_memory_get_dirty_flag(ram_addr_t addr, unsigned client) {
    assert(addr == 0x1234000 && client < DIRTY_MEMORY_NUM);
    calls++;
    bool result = !!(bits & (1u << client));
    if (client == DIRTY_MEMORY_CODE && flips) { bits = flip_to; flips--; }
    return result;
}
'''
C += function(PHYS, "physical_memory_is_clean") + "\n"
C += function(TLB, "notdirty_skip_tlb_cleanup") + "\n"
C += ("static bool cleanup(ram_addr_t ram_addr) { return " +
      condition[1] + "; }\n")
C += r'''
int main(int argc, char **argv) {
    assert(argc == 2);
    if (!strcmp(argv[1], "unset")) {
        unsetenv("XEMU_WASM_NOTDIRTY_FAST");
    } else {
        setenv("XEMU_WASM_NOTDIRTY_FAST", argv[1], 1);
    }
    bool enabled = argv[1][0] != '0';
    for (unsigned state = 0; state <= ALL; state++) {
        bits = state; calls = 0;
        bool original = !physical_memory_is_clean(0x1234000);
        assert(calls == 5);
        calls = 0;
        assert(cleanup(0x1234000) == original);
        assert(original == (state == ALL));
        if (enabled) { assert(calls == ((state & CODE) ? 6 : 1)); }
        else { assert(calls == 5); }
    }
    assert(xemu_wasm_stats.n_notdirty_fast_skip == (enabled ? 16 : 0));
    if (enabled) {
        /* Concurrent unprotection: retaining NOTDIRTY is conservative. */
        bits = ALL & ~CODE; flip_to = ALL; flips = 1; calls = 0;
        assert(!cleanup(0x1234000));
        assert(bits == ALL && calls == 1);
        /* If code becomes protected, the original full check still sees it. */
        bits = ALL; flip_to = ALL & ~CODE; flips = 1; calls = 0;
        assert(!cleanup(0x1234000));
        assert(!(bits & CODE) && calls == 6);
    }
    printf("PASS: notdirty mode=%s, 32 states, conservative transitions\n",
           argv[1]);
}
'''

with tempfile.TemporaryDirectory(prefix="test-wasm-notdirty-") as directory:
    p = Path(directory)
    (p / "test.c").write_text(C)
    subprocess.run(shlex.split(os.environ.get("CC", "cc")) +
                   ["-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror",
                    str(p / "test.c"), "-o", str(p / "test")],
                   check=True, timeout=30)
    for mode in ["unset", "", "0", "1", "2", "invalid"]:
        subprocess.run([str(p / "test"), mode], check=True, timeout=10)
