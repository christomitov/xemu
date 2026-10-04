#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Actual-source telemetry gates/emission; --wasm executes emitted snippets.

Not an emulator/rewind test. Unset is lean (same as TB_STATS=0); TB_STATS=1 restores telemetry.
omits hot counters and defaults phase sampling off; PROFILE=1 restores phases,
PROFILE=0 removes only phases, and nonzero JIT_PROFILE attribution needs phases.
Counts below are Wasm opcodes, not native instructions or throughput estimates.
"""
import argparse
import importlib.util
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location(
    'census_fixture', Path(__file__).with_name('test-wasm-census.py'))
census = importlib.util.module_from_spec(spec)
spec.loader.exec_module(census)
function, section, uleb = census.function, census.section, census.uleb
BACKEND = (ROOT / 'tcg/wasm32/tcg-target.c.inc').read_text()
DRIVER = (ROOT / 'tcg/wasm32.c').read_text()
TCI = (ROOT / 'tcg/tci.c').read_text()
PREFIX = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define EMSCRIPTEN 1
#include "qemu/xemu-wasm-stats.h"
XemuWasmStats xemu_wasm_stats;
const char *volatile xemu_wasm_phase[2];
#define HELPER_PROF_SLOTS 512
static struct { const char *name; uint64_t calls; } helper_prof[512];
'''
CHECK_C = r'''
static void check_c(bool stats, bool phase) {
    int arg = 0;
    assert(xemu_wasm_tb_stats_enabled() == stats);
    assert(xemu_wasm_profile_enabled() == phase);
    XTBSTAT_INC(n_tb_exec);
    XTBSTAT_ADD(n_guest_insn, ++arg);
    assert(xemu_wasm_stats.n_tb_exec == stats);
    assert(xemu_wasm_stats.n_guest_insn == stats && arg == stats);
    XSTAT_INC(n_flip);
    XSTAT_INC(n_sse_checked);
    assert(xemu_wasm_stats.n_flip == 1);
    assert(xemu_wasm_stats.n_sse_checked == 1);
    xemu_wasm_census_hit((3 << XWC_INSN_SHIFT) | XWC_ELIGIBLE | XWC_REGONLY);
    assert(xemu_wasm_stats.n_census_tb_00 == 1);
    assert(xemu_wasm_stats.n_census_insn_40 == 3);
    assert(xemu_wasm_stats.n_census_insn_41 == 3);
    xemu_wasm_phase[0] = "old";
    XTBPHASE_SET("new");
    assert(!strcmp(xemu_wasm_phase[0], phase ? "new" : "old"));
    helper_prof_hit("fixture");
    uint64_t calls = 0;
    for (int i = 0; i < 512; i++) calls += helper_prof[i].calls;
    const char *h = getenv("XEMU_WASM_HELPER_PROF");
    assert(calls == (stats || (h && *h == '1')));
}
/* Deterministic linear addresses for the emitted snippets. */
#define xemu_wasm_phase ((const char *volatile *)(uintptr_t)128)
typedef struct { uint8_t b[1024]; unsigned n; } TCGContext;
typedef TCGContext ByteBuf;
enum { W_I64_LOAD=0x29, W_I64_STORE=0x37, W_I32_STORE=0x36,
       W_I64_ADD=0x7c };
static void wasm8(TCGContext *s, int b) {
    assert(s->n < sizeof(s->b)); s->b[s->n++] = b;
}
static void sleb(TCGContext *s, int64_t v) {
    for (;;) {
        int b = v & 127; v >>= 7;
        bool more = !((v == 0 && !(b & 64)) || (v == -1 && (b & 64)));
        wasm8(s, b | (more ? 128 : 0)); if (!more) break;
    }
}
static void wasm_i32_const(TCGContext *s, int32_t v) {
    wasm8(s, 0x41); sleb(s, v);
}
static void wasm_i64_const(TCGContext *s, int64_t v) {
    wasm8(s, 0x42); sleb(s, v);
}
static void wasm_mem(TCGContext *s, int op, int off) {
    assert(off == 0);
    wasm8(s, op); wasm8(s, op == W_I32_STORE ? 2 : 3); wasm8(s, off);
}
#define bb_u8 wasm8
#define bb_sleb sleb
'''
MAIN = r'''
static void save(const char *path, const char *name, TCGContext *s) {
    char file[1024];
    snprintf(file, sizeof(file), "%s/%s", path, name);
    FILE *f = fopen(file, "wb"); assert(f);
    assert(fwrite(s->b, 1, s->n, f) == s->n);
    assert(fclose(f) == 0); s->n = 0;
}
int main(int argc, char **argv) {
    assert(argc == 4);
    check_c(atoi(argv[2]), atoi(argv[3]));
    uint64_t *ctr = (uint64_t *)(uintptr_t)4096;
    TCGContext s = {0};
    wasm_count(&s, ctr); save(argv[1], "raw", &s);
    wasm_count_tb_stat(&s, ctr); save(argv[1], "counter", &s);
    bb_count(&s, ctr); save(argv[1], "region_counter", &s);
    wasm_phase(&s, NULL); save(argv[1], "phase", &s);
    /* Hot transition instrumentation shapes, not the full routing bodies. */
    for (int i = 0; i < 2; i++) wasm_count_tb_stat(&s, ctr);
    wasm_phase(&s, NULL); save(argv[1], "direct", &s);
    for (int i = 0; i < 3; i++) wasm_count_tb_stat(&s, ctr);
    wasm_phase(&s, NULL); save(argv[1], "indirect", &s);
    wasm_count_tb_stat(&s, ctr);
    wasm_phase(&s, (const char *)(uintptr_t)16);
    wasm_phase(&s, NULL); save(argv[1], "helper", &s);
    return 0;
}
'''


def opcount(code):
    pos = total = 0
    while pos < len(code):
        op = code[pos]
        pos += 1
        total += 1
        assert op in [0x41, 0x42, 0x29, 0x37, 0x36, 0x7c], hex(op)
        immediates = 1 if op in [0x41, 0x42] else 0 if op == 0x7c else 2
        for _ in range(immediates):
            while code[pos] & 128:
                pos += 1
            pos += 1
    return total


def module(code):
    body = b'\0' + code + b'\x0b'
    return (b'\0asm\x01\0\0\0' + section(1, b'\x01\x60\0\0') +
            section(2, b'\x01\x03env\x06memory\x02\0\x01') +
            section(3, b'\x01\0') + section(7, b'\x01\x03run\0\0') +
            section(10, b'\x01' + uleb(len(body)) + body))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--wasm', action='store_true')
    parser.add_argument('--validate', action='store_true',
                        help='validate emitted modules with wasm-validate')
    args = parser.parse_args()
    cases = [(None, None, None, None, 0, 0),
             ('0', None, None, None, 0, 0),
             ('1', '1', None, None, 1, 1),
             ('1', None, None, None, 1, 1),
             ('0', '1', None, None, 0, 1),
             ('1', '0', None, None, 1, 0),
             ('0', '0', '4', None, 0, 1),
             ('0', None, '8', None, 0, 1),
             ('0', None, '0', '1', 0, 0),
             ('invalid', None, None, None, 0, 0),
             ('', None, None, None, 0, 0)]
    text = (PREFIX + function(TCI, 'helper_prof_hit') + CHECK_C +
            function(BACKEND, 'wasm_count') +
            function(BACKEND, 'wasm_count_tb_stat') +
            function(BACKEND, 'wasm_phase') +
            function(DRIVER, 'bb_count') + MAIN)
    with tempfile.TemporaryDirectory(prefix='wasm-tb-stats-') as tmp:
        tmp = Path(tmp)
        src, exe = tmp/'test.c', tmp/'test'
        src.write_text(text)
        subprocess.run([*shlex.split(os.environ.get('CC', 'cc')), '-O2',
                        '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                        '-I'+str(ROOT/'include'), str(src), '-o', str(exe)],
                       check=True)
        baseline = None
        runs = []
        for index, (stats, phase, jit, helper, s, p) in enumerate(cases):
            env = dict(os.environ)
            for key, value in zip(['TB_STATS', 'PROFILE', 'JIT_PROFILE',
                                   'HELPER_PROF'], [stats, phase, jit, helper]):
                key = 'XEMU_WASM_' + key
                env.pop(key, None)
                if value is not None:
                    env[key] = value
            directory = tmp/str(index)
            directory.mkdir()
            subprocess.run([str(exe), str(directory), str(s), str(p)],
                           env=env, check=True)
            codes = {f.name: f.read_bytes() for f in directory.iterdir()}
            assert codes['counter'] == (codes['raw'] if s else b'')
            assert codes['region_counter'] == codes['counter']
            counts = dict(raw=6, counter=6*s, region_counter=6*s, phase=3*p,
                          direct=12*s+3*p, indirect=18*s+3*p, helper=6*s+6*p)
            assert {k: opcount(v) for k, v in codes.items()} == counts
            if index == 0:
                baseline = codes
            elif index == 1:
                assert codes == baseline, 'unset must equal the lean TB_STATS=0 bytes'
            for name, code in codes.items():
                f = directory/(name+'.wasm')
                f.write_bytes(module(code))
                if args.validate:
                    subprocess.run(['wasm-validate', str(f)], check=True)
                n = (1 if name == 'raw' else
                     (dict(counter=1, region_counter=1, direct=2,
                           indirect=3, helper=1).get(name, 0) * s))
                writes_phase = p and name in ['phase', 'direct', 'indirect',
                                             'helper']
                runs.append([str(f), n, bool(writes_phase)])
        if args.wasm:
            import json
            js = tmp/'run.cjs'
            js.write_text('''const fs=require('fs'), assert=require('assert');
const cases=JSON.parse(fs.readFileSync(process.argv[2]));
for(const [file,n,phase] of cases){
  const memory=new WebAssembly.Memory({initial:1});
  const bytes=new Uint8Array(memory.buffer); bytes.fill(0x5a);
  const v=new DataView(memory.buffer), seed=(1n<<32n)-50n;
  v.setBigUint64(4096,seed,true);
  const expected=bytes.slice(), e=new DataView(expected.buffer);
  e.setBigUint64(4096,seed+BigInt(n*100),true);
  if(phase)e.setUint32(128,0,true);
  const code=new WebAssembly.Module(fs.readFileSync(file));
  const m=new WebAssembly.Instance(code,{env:{memory}});
  for(let i=0;i<100;i++)m.exports.run();
  assert.deepEqual(bytes,expected,file);
}
console.log('70 emitted Wasm telemetry snippets: counters/phase/footprint OK');
''')
            manifest = tmp/'runs.json'
            manifest.write_text(json.dumps(runs))
            subprocess.run([*shlex.split(os.environ.get('NODE', 'node')),
                            str(js), str(manifest)], check=True)
        print('10 policy/C-macro fixtures + emitter byte checks passed')
        print('Wasm opcodes removed with TB_STATS=0: entry phase 3; '
              'counter 6; direct transition 15; indirect 21; helper 12')
        if not args.wasm:
            print('No Node execution requested')


if __name__ == '__main__':
    main()
