#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Actual sat16 emitter + helper wrapper, C-helper oracle and Asyncify replay.

CC builds the native emitter fixture; CLANG_WASM builds the original helpers.
NODE, WASM_OPT and WAT2WASM select the test tools. No game runs.
"""
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
BACK = (ROOT / 'tcg/wasm32/tcg-target.c.inc').read_text()
INLINE = (ROOT / 'tcg/wasm32-gvec-sat16.c.inc').read_text()
RUNTIME = (ROOT / 'accel/tcg/tcg-runtime-gvec.c').read_text()


def function(text, name):
    m = re.search(r'(?m)^static [\w *]+\b' + name + r'\([^;]*?\)\n\{', text)
    assert m, name
    end = text.index('\n}', m.end()) + 2
    return text[m.start():end] + '\n'


def uleb(n):
    out = bytearray()
    while n > 127:
        out.append((n & 127) | 128)
        n >>= 7
    return bytes(out + bytes([n]))


def section(tag, data):
    return bytes([tag]) + uleb(len(data)) + data


def name(s):
    b = s.encode()
    return uleb(len(b)) + b


BITS = r'''
static uint32_t extract32(uint32_t x, int s, int n) {
    return (x >> s) & ((1u << n) - 1);
}
static int32_t sextract32(uint32_t x, int s, int n) {
    return (int32_t)(x << (32 - s - n)) >> (32 - n);
}
'''
C = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#define EMSCRIPTEN 1
#define LREG(r) (8 + (r))
#define L32_0 1
#define L32_1 2
#define L32_2 3
#define L32_3 4
#define L64_0 5
#define TCG_TARGET_NB_REGS 16
#define CTX_IDX 0
#define TCG_REG_R0 0
#define TCG_REG_R1 1
#define TCG_AREG0 14
#define TCG_REG_CALL_STACK 15
#define TCG_TARGET_CALL_STACK_OFFSET 0
#define LBLOCK 24
#define BLOCK_PTR_IDX 16
#define WASM_IC_SELF_GLOBAL 17
#define WASM_CTX_REWIND_FUNC_OFF 24
#define HELPER_IDX_START 4
#define CHECK_UNWINDING_IDX 0
#define TCG_CALL_NO_RETURN 1
#define WASM_RELOC_DIRECT_SUSPEND 1
#define BIT(r) (1u << (r))
#define tcg_regset_set_reg(s, r) ((s) |= BIT(r))
#define tcg_regset_test_reg(s, r) ((s) & BIT(r))
#define wasm32_asyncify_state (*(int32_t *)(uintptr_t)64)
typedef uint32_t tcg_insn_unit;
typedef unsigned TCGRegSet;
typedef unsigned TCGReg;
typedef struct {
    const char *name; unsigned typemask, flags;
} TCGHelperInfo;
typedef struct {
    unsigned n; uint8_t b[8192]; unsigned reserved_regs;
    void *reg_to_temp[16];
} TCGContext;
static bool in_snapshot, ic;
static void *wasm_direct_blueprint;
static unsigned block_idx, snapshots;
static void wasm_snapshot_site(int r) { snapshots |= BIT(r); }
static void wasm_add_reloc(unsigned kind, unsigned a, unsigned b) { }
static bool wasm32_ic_enabled(void) { return ic; }
static bool wasm32_unwind_mem_enabled(void) { return true; }
static void wasm8(TCGContext *s, unsigned b) {
    assert(s->n < sizeof(s->b)); s->b[s->n++] = b;
}
'''
C += '\n'.join(re.findall(r'^#define W_\w+\s+0x[0-9a-f]+', BACK, re.M))+'\n'
C += '\n'.join(re.findall(r'^#define dh_typecode_\w+ [0-9]+',
                         (ROOT/'include/exec/helper-head.h.inc').read_text(),
                         re.M))+'\n'
C += BITS
for fn in ['wasm_uleb', 'wasm_sleb', 'wasm_i32_const', 'wasm_i64_const']:
    C += function(BACK, fn)
C += r'''
static void wasm_var(TCGContext *s, unsigned op, unsigned idx) {
    wasm8(s, op); wasm_uleb(s, idx);
}
static void wasm_if_hint(TCGContext *s, unsigned type, bool likely) {
    wasm8(s, W_IF); wasm8(s, type);
}
static void wasm_jit_phase(TCGContext *s, unsigned mask, const char *n) { }
static void wasm_callsite_phase(TCGContext *s, const TCGHelperInfo *i,
                               bool out) { }
static void wasm_phase(TCGContext *s, const char *n) { }
static const char *wasm_helper_phase_name(const TCGHelperInfo *i) {
    return i->name;
}
static void wasm_count_tb_stat(TCGContext *s, void *p) { }
static void wasm_count_helper(TCGContext *s, const void *p, const void *i) { }
static void wasm_count(TCGContext *s, void *p) { }
static struct { unsigned n_helper, n_jit_noreturn; } xemu_wasm_stats;
static bool wasm_profile_exits(void) { return false; }
static bool g_str_has_prefix(const char *n, const char *p) {
    return !strncmp(n, p, strlen(p));
}
static bool g_str_has_suffix(const char *n, const char *p) {
    return strlen(n) >= strlen(p) && !strcmp(n + strlen(n) - strlen(p), p);
}
static int clz32(unsigned n) { return n ? __builtin_clz(n) : 32; }
#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define g_assert_not_reached() abort()
static int get_helper_idx(const void *f) { return 4; }
static int register_helper(const void *f) { abort(); }
static void gen_func_type(const TCGHelperInfo *i) { abort(); }
static void wasm_call_idx(TCGContext *s, unsigned i) {
    wasm8(s, W_CALL); wasm_uleb(s, i);
}
static void wasm_set_tci_tb_ptr(TCGContext *s) { }
#define DECLINE(n) static bool n(TCGContext *s, const tcg_insn_unit *f, \
                                 const TCGHelperInfo *i) { return false; }
DECLINE(wasm_call_lookup_fast)
DECLINE(wasm_call_i128_fast)
DECLINE(wasm_call_sse_fast)
DECLINE(wasm_call_cc_fast)
DECLINE(wasm_call_shufps_inline)
'''
for fn in ['wasm_mem', 'wasm_get_r', 'wasm_get_r32', 'wasm_set_r',
           'helper_nargs', 'wasm_helper_may_suspend', 'wasm_new_block',
           'wasm_new_resumable_block', 'wasm_live_save_enabled',
           'wasm_save_regs_live', 'wasm_save_regs', 'wasm_handle_unwinding',
           'wasm_push_call_args', 'wasm_call_results']:
    C += function(BACK, fn)
C += INLINE + '\n' + function(BACK, 'wasm_call')
C += r'''
int main(int argc, char **argv) {
    assert(argc == 4);
    ic = atoi(argv[2]);
    TCGHelperInfo info = { .name = argv[1],
        .typemask = (6 << 3) | (6 << 6) | (6 << 9) | (2 << 12) };
    if (atoi(argv[3])) {
        /* Wrong ABI or helper name must emit nothing. */
        if (atoi(argv[3]) == 1) {
            info.typemask ^= 1;
        }
        if (!strcmp(info.name, "null")) {
            info.name = NULL;
        }
        TCGContext t = {0};
        assert(!wasm_gvec_sat16_inline(&t, &info) && !t.n);
        puts("{}"); return 0;
    }
    TCGContext s = { .reserved_regs = BIT(13) | BIT(15) };
    s.reg_to_temp[2] = &s;
    /* Prologue: fresh locals, or saved globals, just as the generated B. */
    wasm_var(&s, W_LOCAL_GET, 0); wasm_mem(&s, W_I32_LOAD, 16);
    wasm_if_hint(&s, W_BLOCKTYPE_VOID, true);
    wasm_i64_const(&s, 8192); wasm_set_r(&s, 15);
    wasm_i64_const(&s, 16384); wasm_set_r(&s, 14);
    wasm_i64_const(&s, 0x123456789abcdef); wasm_set_r(&s, 2);
    wasm_i64_const(&s, 0x44446666); wasm_set_r(&s, 4);
    wasm_var(&s, W_LOCAL_GET, 0); wasm_i32_const(&s, 0);
    wasm_mem(&s, W_I32_STORE, 16);
    wasm8(&s, W_ELSE);
    for (unsigned r = 0; r < 16; r++) {
        wasm_var(&s, W_GLOBAL_GET, r); wasm_set_r(&s, r);
    }
    wasm_var(&s, W_GLOBAL_GET, 16); wasm_var(&s, W_LOCAL_SET, LBLOCK);
    wasm8(&s, W_END);
    wasm8(&s, W_BLOCK); wasm8(&s, W_BLOCKTYPE_VOID);
    wasm8(&s, W_BLOCK); wasm8(&s, W_BLOCKTYPE_VOID);
    wasm_var(&s, W_LOCAL_GET, LBLOCK); wasm8(&s, W_I32_WRAP_I64);
    wasm8(&s, W_BR_IF); wasm_uleb(&s, 0); /* rewind skips fresh attempt */
    wasm_call(&s, NULL, &info); /* closes the fresh-prefix block */
    wasm8(&s, W_END);
    wasm_var(&s, W_LOCAL_GET, 0); wasm_get_r(&s, 2);
    wasm_mem(&s, W_I64_STORE, 40);
    wasm_i32_const(&s, 1); wasm8(&s, W_END);
    printf("{\"snapshots\":%u,\"body\":[", snapshots);
    for (unsigned i = 0; i < s.n; i++) printf("%s%u", i ? "," : "", s.b[i]);
    puts("]}");
}
'''


def module(body):
    # helper/check, chain placeholders, original slow helper, generated entry.
    types = b'\x04\x60\x00\x01\x7f\x60\x01\x7f\x01\x7f'
    types += b'\x60\x02\x7f\x7f\x01\x7f\x60\x04\x7f\x7f\x7f\x7f\x00'
    imports = uleb(6)
    for n, ty in [('check', 0), ('s0', 1), ('s1', 1),
                  ('chain', 2), ('slow', 3)]:
        imports += name('env') + name(n) + b'\x00' + uleb(ty)
    imports += name('env') + name('memory') + b'\x02\x00\x02'
    globs = uleb(18) + b'\x7e\x01\x42\x00\x0b' * 17
    globs += b'\x7f\x01\x41\xcd\x00\x0b'  # ic_self = 77
    exports = uleb(4) + name('run') + b'\x00\x05'
    for n, i in [('r4', 4), ('sp', 15), ('block', 16)]:
        exports += name(n) + b'\x03' + uleb(i)
    code = b'\x04\x04\x7f\x02\x7e\x01\x7c\x11\x7e' + body
    return (b'\0asm\x01\0\0\0' + section(1, types) + section(2, imports) +
            section(3, b'\x01\x01') + section(6, globs) + section(7, exports) +
            section(10, b'\x01'+uleb(len(code))+code))


def main():
    with tempfile.TemporaryDirectory(prefix='test-wasm-gvec-sat16-') as tmp:
        tmp = Path(tmp)
        (tmp/'emit.c').write_text(C)
        subprocess.run([*shlex.split(os.environ.get('CC', 'cc')), '-std=gnu11',
                        '-O2', '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-parameter', '-Wno-unused-function',
                        '-Wno-unused-variable', '-I'+str(ROOT/'include'),
                        str(tmp/'emit.c'), '-o', str(tmp/'emit')], check=True)
        cases = []
        for op in ['gvec_ssadd16', 'gvec_sssub16']:
            for live in [0, 1]:
                for ic in [0, 1]:
                    for policy in [None, '', '0', 'invalid', '1', '1yes']:
                        env = dict(os.environ, XEMU_WASM_LIVE_SAVE=str(live))
                        env.pop('XEMU_WASM_GVEC_SSAT16', None)
                        if policy is not None:
                            env['XEMU_WASM_GVEC_SSAT16'] = policy
                        out = json.loads(subprocess.check_output(
                            [str(tmp/'emit'), op, str(ic), '0'], env=env))
                        assert out['snapshots'] == (0xe004 if live else 0xffff)
                        wasm = tmp/f'{len(cases)}.wasm'
                        wasm.write_bytes(module(bytes(out['body'])))
                        subprocess.run(['wasm-validate', str(wasm)], check=True)
                        cases.append(dict(file=str(wasm), op=op, live=live,
                                          ic=ic, enabled=bool(
                                              policy and policy[0] == '1')))
        env = dict(os.environ, XEMU_WASM_GVEC_SSAT16='1')
        for op, mode in [('gvec_ssadd16', '1'), ('gvec_sssub16', '1'),
                         ('gvec_ssadd8', '2'), ('gvec_ssadd32', '2'),
                         ('gvec_usadd16', '2'), ('gvec_mul16', '2'),
                         ('null', '2')]:
            subprocess.run([str(tmp/'emit'), op, '0', mode], env=env,
                           stdout=subprocess.DEVNULL, check=True)
        # Build the unchanged C helpers as the executable oracle.
        oracle = '#include <stdint.h>\n#include <limits.h>\n' + BITS
        oracle += ('#include "tcg/tcg-gvec-desc.h"\n'
                   '#define HELPER(n) helper_##n\n#define unlikely(x) (x)\n')
        oracle += function(RUNTIME, 'clear_high')
        for op in ['gvec_ssadd16', 'gvec_sssub16']:
            begin = RUNTIME.index('void HELPER('+op+')')
            oracle += RUNTIME[begin:RUNTIME.index('\n}', begin)+2]+'\n'
        (tmp/'oracle.c').write_text(oracle)
        subprocess.run([*shlex.split(os.environ.get('CLANG_WASM', 'clang')),
                        '--target=wasm32', '-std=gnu11', '-O2',
                        '-ffreestanding', '-fno-builtin',
                        '-fno-strict-aliasing', '-nostdlib',
                        '-I'+str(ROOT/'include'),
                        str(tmp/'oracle.c'), '-o', str(tmp/'oracle.wasm'),
                        '-Wl,--no-entry,--import-memory,-z,stack-size=0',
                        '-Wl,--export=helper_gvec_ssadd16',
                        '-Wl,--export=helper_gvec_sssub16'],
                       check=True)
        # Real instrumented parent/child around the manual generated-TB bridge.
        (tmp/'shim.wat').write_text('''(module
          (import "env" "memory" (memory 2))
          (import "env" "tb" (func $tb (param i32) (result i32)))
          (import "env" "suspend" (func $suspend))
          (import "env" "oracle" (func $oracle (param i32 i32 i32 i32)))
          (func (export "run") (param i32) (result i32)
            local.get 0 call $tb)
          (func (export "helper") (param i32 i32 i32 i32)
            call $suspend call $suspend
            local.get 0 local.get 1 local.get 2 local.get 3 call $oracle))''')
        subprocess.run([os.environ.get('WAT2WASM', 'wat2wasm'),
                        str(tmp/'shim.wat'), '-o', str(tmp/'shim-raw.wasm')],
                       check=True)
        subprocess.run([os.environ.get('WASM_OPT', 'wasm-opt'),
                        str(tmp/'shim-raw.wasm'), '--asyncify',
                        '--pass-arg=asyncify-imports@env.tb,env.suspend',
                        '-o', str(tmp/'shim.wasm')], check=True)
        (tmp/'cases.json').write_text(json.dumps(cases))
        subprocess.run([*shlex.split(os.environ.get('NODE', 'node')),
                        str(Path(__file__).with_suffix('.mjs')), str(tmp)],
                       check=True)
        print(f'PASS: {len(cases)} policy/LIVE_SAVE/IC configurations '
              '+ ABI/name rejection')


if __name__ == '__main__':
    main()
