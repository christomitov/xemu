#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Actual admission emitter, native Wasm with a mock nonsuspending leaf import.

Checks opt-in/default-off bytes, import typing, tag/alignment/width guards and
profile restoration. Does not execute the production C leaf or Asyncify rewind.
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location(
    'leaf_fixture', Path(__file__).with_name('test-wasm-notdirty-inline.py'))
leaf = importlib.util.module_from_spec(spec)
spec.loader.exec_module(leaf)
function = leaf.function
BACKEND = (ROOT/'tcg/wasm32/tcg-target.c.inc').read_text()
DRIVER = (ROOT/'tcg/wasm32.c').read_text()
# Structural check only: new admission/store remain in the fresh prefix.
store = function(BACKEND, 'wasm_qemu_st')
assert (store.index('wasm_tlb_load(') < store.index('wasm_mem(s, st_op') <
        store.index('wasm_new_resumable_block(') <
        store.index('wasm_save_regs(') < store.index('wasm_handle_unwinding('))
C = leaf.PREFIX + leaf.MEMOP + r'''
#include "exec/memopidx.h"
#define xemu_wasm_phase ((const char *volatile *)(uintptr_t)128)
#define CF_PARALLEL 1
#define TCG_AREG0 14
#define HELPER_IDX_START 0
#define WASM_RELOC_CALL 1
#define WASM_TLB_HINT_MMU_LOCAL 26
#define L32_0 1
#define L32_2 3
#define W_TYPE_I32 0x7f
#define W_I32_LOAD 0x28
#define W_I32_STORE 0x36
#define W_I32_AND 0x71
#define W_I32_OR 0x72
#define W_I32_EQ 0x46
#define W_I32_EQZ 0x45
#define W_ELSE 5
#define W_END 11
#define W_LOCAL_GET 0x20
#define W_LOCAL_SET 0x21
#define W_I32_CONST 0x41
#define XPHASE_VCPU 0
struct TB { unsigned cflags; };
typedef struct {
    struct TB *gen_tb;
    unsigned char b[2048]; unsigned n;
} TCGContext;
typedef struct {
    uint32_t addr_read, addr_write, addr_code, addend;
} CPUTLBEntry;
static unsigned registered, relocs, type_n;
static uint8_t types[16];
static uintptr_t helper_wasm_notdirty(void *env, uint32_t addr, MemOpIdx oi) {
    abort();
}
static int get_helper_idx(void *p) {
    assert(p == helper_wasm_notdirty); return registered ? 0 : -1;
}
static int register_helper(void *p) {
    assert(p == helper_wasm_notdirty && !registered); registered++; return 0;
}
static void types_out8(unsigned v) {
    assert(type_n < sizeof(types)); types[type_n++] = v;
}
static void wasm8(TCGContext *s, unsigned b) {
    assert(s->n < sizeof(s->b)); s->b[s->n++] = b;
}
static void wasm_add_reloc(unsigned kind, unsigned arg, unsigned depth) {
    assert(kind == WASM_RELOC_CALL && arg == 0 && depth == 0); relocs++;
}
'''
for name in ['wasm32_tlb_hint_enabled']:
    C += function(DRIVER, name)
for name in ['wasm_uleb', 'wasm_sleb']:
    C += function(BACKEND, name)
C += r'''
static void wasm_i32_const(TCGContext *s, int32_t v) {
    wasm8(s, W_I32_CONST); wasm_sleb(s, v);
}
static void wasm_var(TCGContext *s, unsigned op, unsigned v) {
    wasm8(s, op); wasm_uleb(s, v);
}
static void wasm_mem(TCGContext *s, unsigned op, unsigned off) {
    wasm8(s, op); wasm_uleb(s, 2); wasm_uleb(s, off);
}
static void wasm_if_hint(TCGContext *s, unsigned ty, bool hint) {
    wasm8(s, 4); wasm8(s, ty);
}
static void wasm_get_r32(TCGContext *s, unsigned reg) {
    assert(reg == TCG_AREG0); wasm_i32_const(s, 4096);
}
'''
for name in ['wasm_tlb_hint_drop', 'wasm_call_idx', 'wasm_jit_profile',
             'wasm_phase', 'wasm_notdirty_inline']:
    C += function(BACKEND, name)
C += r'''
int main(int argc, char **argv) {
    assert(argc == 3);
    struct TB tb = { .cflags = strtoul(argv[2], NULL, 0) };
    TCGContext s = { .gen_tb = &tb };
    bool emitted = wasm_notdirty_inline(&s, strtoul(argv[1], NULL, 0));
    if (!emitted) assert(!s.n && !registered && !relocs && !type_n);
    else {
        const uint8_t expected[] = {0x60,3,0x7f,0x7f,0x7f,1,0x7f};
        assert(registered == 1 && relocs == 1);
        assert(type_n == sizeof(expected) && !memcmp(types, expected, type_n));
    }
    printf("{\"emitted\":%u,\"body\":[", emitted);
    for (unsigned i=0;i<s.n;i++) printf("%s%u", i ? "," : "", s.b[i]);
    puts("]}");
}
'''


def module(code, emitted):
    section, uleb = leaf.census.section, leaf.census.uleb
    # (entry, addr) -> host. Recreate the two scratch locals at the call site.
    body = b'\x01\x19\x7f\x20\x01\x21\x03\x20\x00\x21\x01'
    body += code if emitted else b'\x41\x00'
    body += b'\x0b'
    sig = b'\x60\x02\x7f\x7f\x01\x7f'
    types = (b'\x02\x60\x03\x7f\x7f\x7f\x01\x7f' + sig
             if emitted else b'\x01'+sig)
    imports = (b'\x02\x03env\x04leaf\x00\x00' if emitted else b'\x01')
    imports += b'\x03env\x06memory\x02\x00\x01'
    return (b'\0asm\x01\0\0\0' + section(1, types) + section(2, imports) +
            section(3, b'\x01'+bytes([int(emitted)])) +
            section(7, b'\x01\x03run\x00'+bytes([int(emitted)])) +
            section(10, b'\x01'+uleb(len(body))+body))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--wasm', action='store_true')
    args = parser.parse_args()
    configs = []
    # mode, dump, profile, hint, op, mmu, parallel, admission supported
    for sz in range(3):
        for atom in [0, 5 << 9]:
            for align in [0, 7 << 5]:
                for mmu in [0, 1, 2]:
                    for hint in [0, 1]:
                        for profile in [0, 4, 6]:
                            configs.append(('1', None, profile, hint,
                                            sz | atom | align, mmu, 0, True))
    # unset = default on; any explicit value other than '1' disables
    configs.append((None, None, 0, 0, 2, 0, 0, True))
    configs.append((None, '1234', 0, 0, 2, 0, 0, False))
    for mode in ['', '0', 'invalid']:
        configs.append((mode, None, 0, 0, 2, 0, 0, False))
    for dump in ['', '0', '1234']:
        configs.append(('1', dump, 0, 0, 2, 0, 0, False))
    for op in [3, 4, 2 | 8, 2 | 16, 2 | 256,
               *[2 | (atom << 9) for atom in [1, 2, 3, 4, 6, 7]]]:
        configs.append(('1', None, 0, 0, op, 0, 0, False))
    configs.append(('1', None, 0, 0, 2, 0, 1, False))
    with tempfile.TemporaryDirectory(prefix='notdirty-emitter-') as temp:
        temp = Path(temp)
        source, exe = temp/'test.c', temp/'test'
        source.write_text(C)
        subprocess.run([*shlex.split(os.environ.get('CC', 'cc')), '-std=gnu11',
                        '-O2', '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-parameter', '-Wno-unused-function',
                        '-I'+str(ROOT/'include'), str(source), '-o', str(exe)],
                       check=True)
        # Build-time exclusions must omit the entire call/import, too.
        for guard in ['CONFIG_DEBUG_TCG', 'CONFIG_PLUGIN', 'CONFIG_TSAN']:
            disabled = temp/guard
            subprocess.run([*shlex.split(os.environ.get('CC', 'cc')), '-O2',
                            '-I'+str(ROOT/'include'), '-D'+guard+'=1',
                            str(source), '-o', str(disabled)], check=True)
            env = dict(os.environ, XEMU_WASM_NOTDIRTY_INLINE='1')
            env.pop('XEMU_WASM_SMC_DUMP', None)
            result = json.loads(subprocess.check_output(
                [str(disabled), str(2 << 5), '0'], env=env))
            assert not result['emitted'] and not result['body'], guard
        cases = []
        for i, config in enumerate(configs):
            mode, dump, profile, hint, op, mmu, parallel, expect = config
            env = dict(os.environ)
            for key, value in [('NOTDIRTY_INLINE', mode), ('SMC_DUMP', dump),
                               ('JIT_PROFILE', str(profile)),
                               ('TLB_HINT', str(hint)), ('TB_STATS', '0'),
                               ('PROFILE', '0')]:
                key = 'XEMU_WASM_'+key
                env.pop(key, None)
                if value is not None:
                    env[key] = value
            oi = (op << 5) | mmu
            result = json.loads(subprocess.check_output(
                [str(exe), str(oi), str(parallel)], env=env))
            assert result['emitted'] == expect, configs[i]
            if not expect:
                assert result['body'] == []
            binary = temp/f'{i}.wasm'
            binary.write_bytes(module(bytes(result['body']), expect))
            subprocess.run(['wasm-validate', str(binary)], check=True)
            cases.append([str(binary), oi, expect, bool(profile & 4),
                          bool(profile & 2)])
        if args.wasm:
            js = temp/'run.cjs'
            js.write_text('''const fs=require('fs'), assert=require('assert');
const cases=JSON.parse(fs.readFileSync(process.argv[2]));let probes=0;
for(const [file,oi,enabled,profile,restore] of cases){
  const memory=new WebAssembly.Memory({initial:1});
  const v=new DataView(memory.buffer);
  let calls=0, addr=0, accept=true;
  const code=new WebAssembly.Module(fs.readFileSync(file));
  const m=new WebAssembly.Instance(code,{env:{memory,leaf:(env,a,o)=>{
      assert.equal(env,4096);assert.equal(a>>>0,addr>>>0);assert.equal(o,oi);
      assert.equal(v.getUint32(128,true)!==0x55555555,profile);
      calls++;return accept ? 8192 : 0;
    }}});
  const op=oi>>>5, sz=op&7, a=(op>>>5)&7;
  const alignment=Math.max(sz,a===7 ? sz : a), mask=(1<<alignment)-1;
  for(const page of [0,0x12345000,0xfffff000])
  for(const off of [0,1,2,3,4,7,8,15,16,63,64,4092,4093,4094,4095])
  for(const extra of [0,64,256,384,1,2,4,0x1000])
  for(accept of [false,true]){
    addr=(page+off)>>>0;
    const before=calls, allowed=enabled && extra===0 && !(addr&mask);
    v.setUint32(260,(page|128)^extra,true);v.setUint32(128,0x55555555,true);
    assert.equal(m.exports.run(256,addr),allowed && accept ? 8192 : 0);
    assert.equal(calls-before,allowed ? 1 : 0);
    if(allowed && profile)assert.equal(v.getUint32(128,true)!==0,restore);
    else assert.equal(v.getUint32(128,true),0x55555555);
    probes++;
  }
}
console.log(`PASS: ${probes} actual emitted-Wasm admission probes (mock leaf)`);
''')
            manifest = temp/'cases.json'
            manifest.write_text(json.dumps(cases))
            subprocess.run([*shlex.split(os.environ.get('NODE', 'node')),
                            str(js), str(manifest)], check=True)
        print(f'PASS: {len(configs)} emitter gates/types/default-off checks')
        if not args.wasm:
            print('No Wasm execution requested')


if __name__ == '__main__':
    main()
