#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Test the production phase sampler/export and helper grouping with host C."""

import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
UI = (ROOT / "ui/xemu-wasm.c").read_text()
TCG = (ROOT / "tcg/wasm32/tcg-target.c.inc").read_text()


def function(source, name):
    match = re.search(r"^(?:static|EMSCRIPTEN_KEEPALIVE) [^\n]*\b" +
                      name + r"\(", source, re.M)
    assert match, name
    brace = source.index("{", match.start())
    depth = 0
    for token in re.finditer(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|'
                             r"'(?:\\.|[^'\\])*'|[{}]", source[brace:], re.S):
        if token.group() == "{":
            depth += 1
        elif token.group() == "}":
            depth -= 1
            if depth == 0:
                return source[match.start():brace + token.end()] + "\n"
    raise AssertionError(name)


PREFIX = r'''
#include <assert.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define EMSCRIPTEN_KEEPALIVE
#define XBOX 1
#define qatomic_read(p) __atomic_load_n(p, __ATOMIC_SEQ_CST)
#define qatomic_set(p,v) __atomic_store_n(p, v, __ATOMIC_SEQ_CST)
#define qatomic_inc(p) __atomic_fetch_add(p, 1, __ATOMIC_SEQ_CST)
#define qatomic_xchg(p,v) __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST)
static bool g_str_has_prefix(const char *s, const char *p) {
    return strncmp(s, p, strlen(p)) == 0;
}
static bool g_str_has_suffix(const char *s, const char *p) {
    size_t n = strlen(s), m = strlen(p);
    return n >= m && strcmp(s + n - m, p) == 0;
}
typedef struct TCGHelperInfo { const char *name; } TCGHelperInfo;
'''

SUFFIX = r'''
static void reset(void) {
    memset(phase_prof, 0, sizeof(phase_prof));
    memset(phase_overflow, 0, sizeof(phase_overflow));
}
static uint64_t total(const char *s, int t) {
    uint64_t n = 0;
    if (t) s = strchr(s, '\n') + 1;
    s = strchr(s, '|') + 1;
    while (*s && *s != '\n') {
        s = strchr(s, '='); assert(s);
        n += strtoull(s + 1, NULL, 10);
        s = strchr(s, ';'); assert(s); s++;
    }
    return n;
}
static bool done;
static void *writer(void *opaque) {
    for (unsigned i = 0; i < 500000; i++) phase_bump(0, "jit");
    qatomic_set(&done, true);
    return NULL;
}
static void group(const char *in, const char *want) {
    TCGHelperInfo info = { in };
    const char *got = wasm_helper_phase_name(&info);
    assert((!want && !got) || (want && got && !strcmp(want, got)));
}
int main(void) {
    group(NULL, NULL);
    group("fadd_ST0_FT0", "x87");
    group("fmul_STN_ST0__soft", "x87");
    group("fldl_ST0", "x87"); group("fsts_ST0", "x87");
    group("flush_page", "flush_page");
    group("fxsave", "fpu_state"); group("xrstor", "fpu_state");
    group("emms", "mmx"); group("femms", "mmx");
    group("paddsw_mmx", "mmx"); group("addss", "addss");
    group("gvec_ssadd16", "gvec"); group("gvec_sssub16", "gvec");
    group("rdtsc", "rdtsc");
    char names[PHASE_SLOTS + 40][32];
    for (int i = 0; i < PHASE_SLOTS + 40; i++) {
        snprintf(names[i], sizeof(names[i]), "helper_%d", i);
        phase_bump(0, names[i]);
    }
    for (int i = 0; i < 7; i++) phase_bump(1, "idle");
    const char *s = xemu_wasm_phase_top();
    assert(total(s, 0) == PHASE_SLOTS + 40 && total(s, 1) == 7);
    assert(strstr(s, "phase_other=") && strstr(s, "phase_overflow=40;"));
    s = xemu_wasm_phase_top();
    assert(total(s, 0) == 0 && total(s, 1) == 0);
    phase_bump(0, names[0]); phase_bump(0, "late helper");
    s = xemu_wasm_phase_top();
    assert(total(s, 0) == 2 && strstr(s, "phase_overflow=1;"));
    reset();
    /* A name that cannot fit must not discard its count or the GPU row. */
    char large[10001]; memset(large, 'L', sizeof(large) - 1);
    large[sizeof(large) - 1] = 0;
    phase_bump(0, large); phase_bump(1, "idle");
    s = xemu_wasm_phase_top();
    assert(total(s, 0) == 1 && total(s, 1) == 1);
    assert(strstr(s, "phase_other=1;"));
    reset();
    pthread_t thread;
    assert(pthread_create(&thread, NULL, writer, NULL) == 0);
    uint64_t sum = 0;
    do { sum += total(xemu_wasm_phase_top(), 0); } while (!qatomic_read(&done));
    assert(pthread_join(thread, NULL) == 0);
    sum += total(xemu_wasm_phase_top(), 0);
    assert(sum == 500000);
    puts("PASS: helper grouping, top-k/overflow/long-name accounting, "
         "500000 concurrent samples retained");
    return 0;
}
'''


def main():
    start = UI.index("#define PHASE_SLOTS")
    end = UI.index("static void phase_bump", start)
    code = (PREFIX + UI[start:end] + function(UI, "phase_bump") +
            function(UI, "xemu_wasm_phase_top") +
            function(TCG, "wasm_helper_phase_name") + SUFFIX)
    with tempfile.TemporaryDirectory(prefix="test-wasm-phase-") as tmp:
        source = Path(tmp) / "test.c"
        exe = Path(tmp) / "test"
        source.write_text(code)
        subprocess.run([*shlex.split(os.environ.get("CC", "cc")),
                        "-std=gnu11", "-O2", "-pthread", str(source),
                        "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True, timeout=30)


if __name__ == "__main__":
    main()
