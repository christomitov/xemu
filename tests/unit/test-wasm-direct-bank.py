#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Resident architectural-bank emitter probes (not production region routing).

A fixture-controlled br_table visits bodies in arbitrary runtime order. Compare
against the independently IA32-tested standalone emitter after every body, and
check that canonical env is untouched until the final active boundary. The
observer is test-only and does not re-enter any Wasm while the bank is running.
No full-emulator, IRQ, live-link, suspension or performance claim is made here.
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import random
import shlex
import struct
import subprocess
import tempfile

SPEC = importlib.util.spec_from_file_location(
    'direct', Path(__file__).with_name('test-wasm-direct.py'))
D = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(D)
ROOT = D.ROOT

C = D.C[:D.C.index('int main(void)')] + r'''
#include "tcg/wasm32-direct-bank.c.inc"
static void output(XwdCode *code) {
 assert(code->valid);
 assert(fwrite(&code->size,4,1,stdout)==1);
 assert(fwrite(code->bytes,1,code->size,stdout)==code->size);
}
int main(void) {
 check_capture();
 unsigned n; assert(scanf("%u",&n)==1 && n>0 && n<=32);
 XwdPlan p[32];
 uint32_t constants[]={''' + ','.join(D.CC_NAMES) + r'''};
 assert(fwrite(constants,sizeof(constants),1,stdout)==1);
 for(unsigned i=0;i<n;i++) {
  XwdCapture cap={.version=XWD_VERSION,.valid=true,.pc=0xfffffff0u+i*16};
  unsigned size,count,fall,relative;
  assert(scanf("%u%u%u%u",&size,&count,&fall,&relative)==4);
  assert(size<=XWD_MAX_BYTES && count<=XWD_MAX_INSNS);
  cap.size=size;cap.count=count;cap.fallthrough=fall;cap.pcrel=relative;
  for(unsigned j=0;j<count;j++) {
   unsigned v;assert(scanf("%u",&v)==1);cap.end[j]=v;
  }
  for(unsigned j=0;j<size;j++) {
   unsigned v;assert(scanf("%x",&v)==1);cap.bytes[j]=v;
  }
  assert(xwd_decode(&cap,&p[i]));
  XwdCode code;assert(xwd_emit(&p[i],&layout,false,8192,0,&code));output(&code);
 }
 for(unsigned bias=0;bias<=32;bias+=32) {
  for(unsigned verify=0;verify<2;verify++) {
   XwdCode code={.valid=true};
   XwdEmitter e={.code=&code,.layout=&layout,.cc_helper=0,.local_bias=bias};
   /* Last private local: fixture iteration index (23). With relocation,
    * preserve the actual TCG mixed i32/i64/f64 local types at slots 1..24. */
   if(bias) {
    const uint8_t decl[]={5,4,0x7f,2,0x7e,1,0x7c,17,0x7e,31,0x7f};
    for(unsigned j=0;j<sizeof(decl);j++)xwd_byte(&e,decl[j]);
   }else{
    xwd_byte(&e,1);xwd_uleb(&e,23);xwd_byte(&e,0x7f);
   }
   xwd_bank_init(&e);
   xwd_byte(&e,0x02);xwd_byte(&e,0x40); /* outer block */
   xwd_byte(&e,0x03);xwd_byte(&e,0x40); /* member loop */
   xwd_local(&e,0x20,23);xwd_const(&e,6000);xwd_mem(&e,0x28,0);
   xwd_byte(&e,0x46);xwd_byte(&e,0x0d);xwd_uleb(&e,1); /* done */
   for(unsigned i=0;i<n;i++) {xwd_byte(&e,0x02);xwd_byte(&e,0x40);}
   xwd_const(&e,6004);xwd_local(&e,0x20,23);xwd_const(&e,2);
   xwd_byte(&e,0x74);xwd_byte(&e,0x6a);xwd_mem(&e,0x28,0);
   xwd_byte(&e,0x0e);xwd_uleb(&e,n);
   for(unsigned i=0;i<n;i++)xwd_uleb(&e,i);
   xwd_uleb(&e,0);
   for(unsigned i=0;i<n;i++) {
    xwd_byte(&e,0x0b);
    assert(xwd_bank_step(&e,&p[i],verify));
    xwd_bank_store(&e,true,8192); /* test-only per-member observation */
    if(p[i].conditional) xwd_local(&e,0x20,XD_TAKEN);
    else xwd_const(&e,2); /* not a conditional edge */
    xwd_byte(&e,0x10);xwd_uleb(&e,1); /* fixture observer */
    xwd_local(&e,0x20,23);xwd_const(&e,1);xwd_byte(&e,0x6a);
    xwd_local(&e,0x21,23);
    xwd_byte(&e,0x0c);xwd_uleb(&e,n-1-i);
   }
   xwd_byte(&e,0x0b);xwd_byte(&e,0x0b);
   xwd_bank_store(&e,verify,8192);
   xwd_const(&e,0);xwd_byte(&e,0x0b);output(&code);
  }
 }
 /* Contract and capacity negatives do not produce an executable body. */
 XwdCode bad={.valid=true};XwdEmitter e={.code=&bad,.layout=&layout};
 assert(!xwd_bank_step(&e,&p[0],false));
 e.resident=true;e.valid=0xff;e.cc_helper=UINT32_MAX;
 XwdPlan invalid=p[0];invalid.needs_flags=true;
 assert(!xwd_bank_step(&e,&invalid,false));
 invalid=p[0];invalid.version=0;assert(!xwd_bank_step(&e,&invalid,false));
 e.cc_helper=0;bad.size=XWD_MAX_CODE;
 assert(!xwd_bank_step(&e,&p[0],false) && !bad.valid);
}
'''


def module(body):
    return (b'\0asm\x01\0\0\0' +
        D.section(1, b'\x03\x60\x01\x7f\x01\x7f'
                     b'\x60\x04\x7f\x7f\x7f\x7f\x01\x7f'
                     b'\x60\x01\x7f\x00') +
        D.section(2, b'\x03\x03env\x06memory\x02\x00\x01'
                     b'\x06helper\x02cc\x00\x01'
                     b'\x05probe\x04tick\x00\x02') +
        D.section(3, b'\x01\x00') +
        D.section(7, b'\x01\x05start\x00\x02') +
        D.section(10, b'\x01' + D.uleb(len(body)) + body))


JS = r'''
const fs=require('fs'),assert=require('assert');
const d=JSON.parse(fs.readFileSync(process.argv[2]));
const cc=new WebAssembly.Instance(new WebAssembly.Module(
 fs.readFileSync(d.flagsWasm)),{}).exports.helper_cc_compute_all;
const memory=new WebAssembly.Memory({initial:1});
const b=new Uint8Array(memory.buffer),v=new DataView(memory.buffer);
const get=a=>v.getUint32(a,true),set=(a,x)=>v.setUint32(a,x,true);
let trace,initial,shadow;
const imports={env:{memory},helper:{cc},probe:{tick(taken){
 // No nested Wasm calls. Env must still be the unspilled entry packet.
 assert.deepEqual(b.slice(4096,4152),initial,'no internal canonical spill');
 assert.equal(b[4092],shadow?0x5a:1);
 trace.push({regs:Array.from({length:8},(_,i)=>get(8192+4*i)),
             lazy:Array.from({length:4},(_,i)=>get(8224+4*i)),
             pc:get(8240),taken});
}}};
const instantiate=p=>new WebAssembly.Instance(new WebAssembly.Module(
 fs.readFileSync(p)),imports).exports.start;
const refs=d.refs.map(instantiate),banks=d.banks.map(instantiate);
function reset(test){
 b.fill(0x5a);set(128,4096);
 test.regs.forEach((x,i)=>set(4096+4*i,x));set(4128,test.pc);
 test.lazy.forEach((x,i)=>set(4136+4*i,x));
 set(6000,test.route.length);test.route.forEach((x,i)=>set(6004+4*i,x));
 initial=b.slice(4096,4152);
}
let calls=0,members=0;
for(const test of d.tests){
 reset(test);const want=[];
 for(const k of test.route){
  refs[k](128);
  const regs=Array.from({length:8},(_,i)=>get(4096+4*i));
  const flags=cc(get(4136),get(4140),get(4144),get(4148))&0x8d5;
  const pc=get(4128);
  // Last Jcc observes the resulting status flags (Jcc does not change them).
  let taken=2;
  if(d.cond[k]!==null){
   const c=d.cond[k],base=c>>1,of=!!(flags&2048),sf=!!(flags&128);
   const zf=!!(flags&64),cf=!!(flags&1),pf=!!(flags&4);
   taken=+[of,cf,zf,cf||zf,sf,pf,sf!==of,zf||(sf!==of)][base];
   if(c&1)taken^=1;
  }
  want.push({regs,flags,pc,taken});
 }
 for(let i=0;i<banks.length;i++){
  reset(test);shadow=!!(i&1);trace=[];const before=b.slice();
  assert.equal(banks[i](128),0);assert.equal(trace.length,want.length);
  trace.forEach((s,j)=>{
   const w=want[j];assert.deepEqual(s.regs,w.regs,'retained GPRs');
   assert.equal(cc(...s.lazy)&0x8d5,w.flags,'runtime lazy-CC opcode');
   assert.equal(s.pc,w.pc,'retained/wrapped PC');
   assert.equal(s.taken,w.taken,'selected Jcc slot, including equal PCs');
  });
  const last=want.at(-1),out=shadow?8192:4096;
  for(let r=0;r<8;r++)assert.equal(get(out+4*r),last.regs[r]);
  const a=shadow?out+32:out+40;
  assert.equal(cc(get(a),get(a+4),get(a+8),get(a+12))&0x8d5,last.flags);
  assert.equal(get(out+(shadow?48:32)),last.pc);
  // Scratch snapshots and optional sampler word are the only extra writes.
  b.set(before.subarray(8192,8244),8192);
  if(!shadow){b.set(before.subarray(4096,4152),4096);b[4092]=before[4092];}
  if(get(9000)!==0x5a5a5a5a){
   assert.equal(get(9000),0x2222);b.set(before.subarray(9000,9004),9000);
  }
  assert.deepEqual(b,before,'only boundary/scratch/phase writes');
  calls++;members+=trace.length;
 }
}
console.log(JSON.stringify({calls,members,status:'PASS',
 scope:'emitted bank; fixture routing; standalone-emitter comparison'}));
'''


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--wasm', action='store_true')
    args = ap.parse_args()
    rng = random.Random(0x524547)
    producers = [['f9'], ['f8'], ['f5'], ['01d8'], ['29cb'], ['31f6'],
                 ['6683d8ff'], ['fecc'], ['83d17f'], ['6683db80'],
                 ['87e7'], ['0fb7d4'], ['8d4488ff'], ['f7d0']]
    cases = []
    for i in range(16):
        # Last displacement 0 deliberately makes taken/fall PCs equal.
        seq = producers[i % len(producers)] + ['660f44fd', '80d480',
              'f5', '0f92c6', '66ffc3', f'{0x70+i:02x}00']
        if i & 1:
            seq = producers[i % len(producers)] + ['90', f'{0x70+i:02x}80']
        cases.append(([bytes.fromhex(s) for s in seq], bool(i & 2), i))
    cases += [([bytes.fromhex(s) for s in seq], bool(i & 1), None)
              for i, seq in enumerate(producers)]
    lines = [str(len(cases))]
    for seq, relative, cond in cases:
        ends = []; raw = b''
        for insn in seq:
            raw += insn; ends.append(len(raw))
        lines += [f'{len(raw)} {len(seq)} {int(cond is None)} {int(relative)}',
                  ' '.join(map(str, ends)), raw.hex(' ')]
    with tempfile.TemporaryDirectory(prefix='wasm-direct-bank-') as td:
        tmp = Path(td); src = tmp/'emit.c'; src.write_text(C); exe = tmp/'emit'
        subprocess.run([*shlex.split(os.environ.get('CC', 'cc')), '-O2',
            '-std=gnu11', '-Wall', '-Wextra', '-Werror',
            '-I'+str(ROOT/'include'), '-I'+str(ROOT), str(src), '-o', str(exe)],
            check=True)
        output = subprocess.check_output([str(exe)],
            input='\n'.join(lines).encode())
        constants = struct.unpack_from(f'<{len(D.CC_NAMES)}I', output)
        pos = len(constants)*4; paths = []
        for i in range(len(cases)+4):
            size, = struct.unpack_from('<I', output, pos); pos += 4
            p = tmp/f'{i}.wasm'; body = output[pos:pos+size]; pos += size
            p.write_bytes(D.module(body) if i < len(cases) else module(body))
            paths.append(str(p))
        assert pos == len(output)
        tests = []
        for i in range(192):
            lazy = [rng.getrandbits(32), rng.getrandbits(32), i & 1,
                    constants[i % len(constants)]]
            if lazy[3] in constants[16:22]:
                lazy[1] = i & 1
            tests.append(dict(regs=[rng.getrandbits(32) for _ in range(8)],
                lazy=lazy, pc=0xfffffff0 if i % 3 else rng.getrandbits(32),
                route=([i % len(cases)] * 32 if i < 32 else
                       [rng.randrange(len(cases)) for _ in range(64)])))
        flags = tmp/'flags.wasm'; data = tmp/'data.json'
        data.write_text(json.dumps(dict(refs=paths[:len(cases)],
            banks=paths[len(cases):], flagsWasm=str(flags), tests=tests,
            cond=[c for _, _, c in cases])))
        if args.wasm:
            source = tmp/'flags.c'; source.write_text(D.FLAGS_C)
            clang = os.environ.get('WASM_CLANG', str(ROOT.parent /
                'emsdk/upstream/bin/clang'))
            subprocess.run([clang, '--target=wasm32', '-O2', '-nostdlib',
                '-Wl,--no-entry', '-Wl,--export=helper_cc_compute_all',
                '-I'+str(ROOT), str(source), '-o', str(flags)], check=True)
            runner = tmp/'run.cjs'; runner.write_text(JS)
            subprocess.run([*shlex.split(os.environ.get('NODE', 'node')),
                str(runner), str(data)], check=True)
        print(f'{len(cases)} member bodies + 4 resident-bank bodies; '
              'contract/capacity negatives passed; ' +
              ('Wasm executed' if args.wasm else 'Wasm NOT executed'))


if __name__ == '__main__':
    main()
