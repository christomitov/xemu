#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Actual-source direct verifier and emitted edge-PC/import/phase contracts.

Mock CPU descriptors/emitter adapters, not a full TCG or Asyncify execution.
Run with --wasm for the six emitted verifier-wrapper paths.
"""
import argparse
import json
import os
from pathlib import Path
import runpy
import resource
import shlex
import struct
import subprocess
import tempfile

D = runpy.run_path(str(Path(__file__).with_name('test-wasm-direct.py')))
ROOT = D['ROOT']
GLUE = (ROOT / 'tcg/wasm32-direct-glue.c.inc').read_text()

C = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "qemu/xemu-wasm-direct.h"
''' + D['FLAGS_C'] + r'''
typedef struct CPUState { struct { bool can_do_io; } neg; } CPUState;
typedef struct CPUX86State {
 uint32_t regs[8],eip,cc_dst,cc_src,cc_src2,cc_op;
 CPUState cpu;
} CPUX86State;
#define env_cpu(env) (&(env)->cpu)
static struct { unsigned n_direct_checked,n_direct_mismatch,
 n_direct_flags_checked,n_direct_jcc_checked; } stats;
#define XSTAT_INC(name) (++stats.name)
''' + D['frontend_function']('x86_wasm_direct_verify') + r'''
static void test_verify(unsigned bad) {
 CPUX86State env={.eip=0xdeadbeef,.cc_op=CC_OP_EFLAGS,.cc_src=0x85,
                  .cpu.neg.can_do_io=true};
 XwdState state={.eip=0x12345678,.cc_op=CC_OP_EFLAGS,.cc_src=0x85,
                 .pending=1,.features=3};
 if(bad==1)env.regs[7]=1;
 if(bad==2)env.cc_src^=1;
 if(bad==3)state.eip^=1;
 if(bad==4)env.cpu.neg.can_do_io=false;
 /* Canonical chosen PC may deliberately differ from stale env->eip. */
 x86_wasm_direct_verify(&env,&state,1,0x12345678);
 assert(!bad && !state.pending && stats.n_direct_checked==1 &&
        stats.n_direct_flags_checked==1 && stats.n_direct_jcc_checked==1);
 env.regs[0]=1;env.cpu.neg.can_do_io=false;
 x86_wasm_direct_verify(&env,&state,1,0); /* duplicate/guard: no pending */
 assert(stats.n_direct_checked==1 && !stats.n_direct_mismatch);
}
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "tcg/wasm32-direct.c.inc"
#pragma GCC diagnostic pop
typedef struct TCGContext { XwdEmitter e; } TCGContext;
static XwdTranslation *wasm_direct_translation;
#define wasm_direct_expected (*(XwdState *)(uintptr_t)8192)
static int direct_mode;
#define xemu_wasm_direct_mode() direct_mode
#define tcg_debug_assert assert
#define TCG_AREG0 0
#define W_TYPE_I32 0x7f
#define W_I32_LOAD 0x28
#define W_CALL 0x10
static bool registered;
static unsigned types_len;
static uint8_t types[16];
static int get_helper_idx(const void *f) { (void)f;return registered?0:-1; }
static int register_helper(const void *f) { (void)f;registered=true;return 0; }
static void types_out8(unsigned b) {
 assert(types_len<16);types[types_len++]=b;
}
static void wasm_i32_const(TCGContext *s,uint32_t v) { xwd_const(&s->e,v); }
static void wasm_get_r32(TCGContext *s,int reg) {
 assert(reg==TCG_AREG0);xwd_local(&s->e,0x20,0);xwd_mem(&s->e,0x28,0);
}
static void wasm_mem(TCGContext *s,int op,uint32_t off) {xwd_mem(&s->e,op,off);}
static void wasm_call_idx(TCGContext *s,unsigned idx) {
 xwd_byte(&s->e,W_CALL);xwd_uleb(&s->e,idx);
}
static void wasm_phase(TCGContext *s,const char *name) {
 xwd_const(&s->e,9000);xwd_const(&s->e,name?0x3333:0x2222);
 xwd_mem(&s->e,0x36,0);
}
''' + D['c_function'](GLUE, 'wasm_direct_verify') + r'''
int main(int argc,char **argv) {
 test_verify(argc>1?atoi(argv[1]):0);
 for(unsigned i=0;i<6;i++) {
  XwdTranslation t={.layout={.eip=32,.verify=x86_wasm_direct_verify},
    .capture={.pcrel=i==2,.jump_valid=3,.jump_pc={0xffffffff,0x80000000}}};
  XwdCode code={.valid=true};TCGContext s={.e={.code=&code}};
  direct_mode=i==4?1:2;wasm_direct_translation=i==5?NULL:&t;
  registered=false;types_len=0;
  xwd_byte(&s.e,0); /* no private locals */
  wasm_direct_verify(&s,i==3?-1:i==1?1:0);
  xwd_const(&s.e,0);xwd_byte(&s.e,0x0b);
  const uint8_t expected[]={0x60,4,0x7f,0x7f,0x7f,0x7f,0};
  assert(types_len==(i<4?sizeof(expected):0));
  if(i<4)assert(!memcmp(types,expected,sizeof(expected)));
  assert(fwrite(&code.size,4,1,stdout)==1);
  assert(fwrite(code.bytes,code.size,1,stdout)==1);
 }
}
'''

JS = r'''
const fs=require('fs'),assert=require('assert');
const paths=JSON.parse(fs.readFileSync(process.argv[2]));
const memory=new WebAssembly.Memory({initial:1});
const bytes=new Uint8Array(memory.buffer),v=new DataView(memory.buffer);
for(let i=0;i<6;i++){
 let calls=[];bytes.fill(0x5a);
 v.setUint32(128,4096,true);v.setUint32(4128,0x12345678,true);
 const before=bytes.slice();
 const verify=(...args)=>{
  assert.equal(v.getUint32(9000,true),0x3333,'verifier phase');
  calls.push(args.map(x=>x>>>0));
 };
 const mod=new WebAssembly.Instance(new WebAssembly.Module(
   fs.readFileSync(paths[i])),{env:{memory},helper:{verify}});
 assert.equal(mod.exports.start(128),0);
 const pc=[0xffffffff,0x80000000,0x12345678,0x12345678][i];
 assert.deepEqual(calls,i<4?[[4096,8192,1,pc]]:[]);
 assert.equal(v.getUint32(9000,true),i<4?0x2222:0x5a5a5a5a);
 bytes.set(before.subarray(9000,9004),9000);
 assert.deepEqual(bytes,before,'no architectural writes from verifier glue');
}
console.log('6 emitted verifier edge-PC/import/phase paths passed');
'''


def module(body):
    section, uleb = D['section'], D['uleb']
    return (b'\0asm\x01\0\0\0' +
            section(1, b'\x02\x60\x01\x7f\x01\x7f'
                       b'\x60\x04\x7f\x7f\x7f\x7f\x00') +
            section(2, b'\x02\x03env\x06memory\x02\x00\x01'
                       b'\x06helper\x06verify\x00\x01') +
            section(3, b'\x01\x00') +
            section(7, b'\x01\x05start\x00\x01') +
            section(10, b'\x01' + uleb(len(body)) + body))


def check_policy(tmp):
    src = tmp/'policy.c'
    src.write_text('#include <stdint.h>\n#include <stdbool.h>\n'
                   '#include <stdlib.h>\n#include <stdio.h>\n'
                   '#include "qemu/xemu-wasm-direct.h"\n'
                   'int main(void) {printf("%d %d\\n", '
                   'xemu_wasm_direct_mode(), '
                   'xemu_wasm_direct_flags_enabled());}\n')
    exe = tmp/'policy'
    subprocess.run([*shlex.split(os.environ.get('CC','cc')), '-O2',
        '-DEMSCRIPTEN', '-DCONFIG_TCG_WASM_JIT', '-I'+str(ROOT/'include'),
        str(src), '-o', str(exe)], check=True)
    env = {k:v for k,v in os.environ.items()
           if k not in ['XEMU_WASM_DIRECT_X86','XEMU_WASM_DIRECT_FLAGS']}
    for mode in [None, '', '0', '1', '2', '3']:
        for flags in [None, '', '0', '1', 'bogus']:
            current = dict(env)
            if mode is not None:
                current['XEMU_WASM_DIRECT_X86'] = mode
            if flags is not None:
                current['XEMU_WASM_DIRECT_FLAGS'] = flags
            result = subprocess.check_output([str(exe)], env=current).decode()
            expected = (f'{int(mode) if mode in ["1","2"] else 0} '
                        f'{int(flags == "1")}\n')
            assert result == expected
    print('30 actual-source mode/flags policy cases passed')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--wasm', action='store_true')
    args = ap.parse_args()
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    with tempfile.TemporaryDirectory(prefix='wasm-direct-glue-') as td:
        tmp = Path(td)
        check_policy(tmp)
        src = tmp/'glue.c'; src.write_text(C)
        exe = tmp/'glue'
        subprocess.run([*shlex.split(os.environ.get('CC', 'cc')), '-O2',
            '-std=gnu11', '-Wall', '-Wextra', '-Werror',
            '-I'+str(ROOT/'include'), '-I'+str(ROOT), str(src), '-o', str(exe)],
            check=True)
        output = subprocess.check_output([str(exe)])
        for bad in range(1,5):
            failed = subprocess.run([str(exe),str(bad)],capture_output=True)
            assert failed.returncode != 0
            assert b'differential mismatch' in failed.stderr
        off = 0; paths = []
        for i in range(6):
            size, = struct.unpack_from('<I',output,off); off += 4
            binary = tmp/f'{i}.wasm'
            binary.write_bytes(module(output[off:off+size])); off += size
            paths.append(str(binary))
        assert off == len(output)
        if args.wasm:
            script = tmp/'run.cjs'; script.write_text(JS)
            manifest = tmp/'cases.json'; manifest.write_text(json.dumps(paths))
            subprocess.run([*shlex.split(os.environ.get('NODE','node')),
                str(script),str(manifest)],check=True)
        print('actual C verifier: stale env PC accepted via canonical edge PC; '
              'GPR/CC/PC/IO mismatches rejected; '
              'pending/coverage checks passed')


if __name__ == '__main__':
    main()
