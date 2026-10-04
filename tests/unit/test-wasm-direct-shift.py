#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Direct x86 register shifts (SHL/SAL, SHR, SAR by imm8 or 1).

Oracle: the original translation's semantics (target/i386/tcg/emit.c.inc
gen_SHL/SHR/SAR): T0 (sign-extended for SAR), cc_src = T0 shifted by
count - 1, cc_dst = result, CC_OP_SHL / CC_OP_SAR; masked count 0 changes
nothing. Flags are compared through the actual lazy-CC helper, not native
hardware (AF, and OF for count > 1, are undefined on real CPUs).
"""
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
int main(void) {
 check_capture();
 assert(fwrite(layout.cc_shl,12,1,stdout)==1);
 assert(fwrite(layout.cc_sar,12,1,stdout)==1);
 unsigned cases; assert(scanf("%u",&cases)==1);
 for(unsigned i=0;i<cases;i++) {
  XwdCapture cap={.version=XWD_VERSION,.valid=true,.pc=0x40000000,
                  .count=1,.fallthrough=true};
  unsigned size; assert(scanf("%u",&size)==1);
  cap.size=size; cap.end[0]=size;
  for(unsigned j=0;j<size;j++){unsigned v;assert(scanf("%x",&v)==1);cap.bytes[j]=v;}
  XwdPlan p; uint32_t ok=xwd_decode(&cap,&p);
  assert(fwrite(&ok,4,1,stdout)==1);
  if(!ok)continue;
  for(int verify=0;verify<2;verify++){
   XwdCode code; assert(xwd_emit(&p,&layout,verify,8192,0,&code));
   assert(fwrite(&code.size,4,1,stdout)==1);
   assert(fwrite(code.bytes,1,code.size,stdout)==code.size);
  }
 }
}
'''

JS = r'''
const fs=require('fs'), assert=require('assert');
const d=JSON.parse(fs.readFileSync(process.argv[2]));
const memory=new WebAssembly.Memory({initial:1});
const v=new DataView(memory.buffer), bytes=new Uint8Array(memory.buffer);
const flags=new WebAssembly.Instance(new WebAssembly.Module(
 fs.readFileSync(d.flagsWasm)),{}).exports.helper_cc_compute_all;
const ENV=4096; let probes=0;
const get=(r,n,w)=>w===8?(n>=4?(r[n&3]>>>8)&0xff:r[n]&0xff):w===16?r[n]&0xffff:r[n]>>>0;
function put(r,n,w,val){
 if(w===32){r[n]=val>>>0;return;}
 if(w===16){r[n]=((r[n]&0xffff0000)|(val&0xffff))>>>0;return;}
 const s=n>=4?8:0,k=n&3;r[k]=((r[k]&~(0xff<<s))|((val&0xff)<<s))>>>0;
}
for(const c of d.cases){
 const mods=c.code.map(p=>new WebAssembly.Instance(new WebAssembly.Module(
  fs.readFileSync(p)),{env:{memory},helper:{cc:flags}}));
 for(const t of c.tests){
  // expected
  const r=t.regs.slice(), w=c.width;
  let T0=get(r,c.dst,w);
  if(c.op==='SAR'&&w<32)T0=w===8?(T0<<24)>>24:(T0<<16)>>16;
  const sh=(x,n)=>c.op==='SHL'?(x<<n):c.op==='SHR'?(x>>>n):(x>>n);
  const cs=sh(T0,c.count-1)>>>0, cd=sh(T0,c.count)>>>0;
  put(r,c.dst,w,cd);
  const ccop=c.op==='SHL'?d.shl[[8,16,32].indexOf(w)]:d.sar[[8,16,32].indexOf(w)];
  const want=flags(cd,cs,0,ccop)&0x8d5;
  for(let verify=0;verify<2;verify++){
   bytes.fill(0);v.setUint32(128,ENV,true);
   for(let i=0;i<8;i++)v.setUint32(ENV+4*i,t.regs[i],true);
   v.setUint32(ENV+32,0x40000000,true);
   v.setUint32(ENV+40,0x1234,true);v.setUint32(ENV+44,0x55,true);
   v.setUint32(ENV+52,1,true);
   assert.equal(mods[verify].exports.start(128),0);
   const out=verify?8192:ENV, cc=verify?out+32:out+40;
   for(let i=0;i<8;i++)assert.equal(v.getUint32(out+4*i,true),r[i],
     c.hex+' GPR'+i+' verify='+verify);
   const got=flags(v.getUint32(cc,true),v.getUint32(cc+4,true),
     v.getUint32(cc+8,true),v.getUint32(cc+12,true))&0x8d5;
   assert.equal(got,want,c.hex+' flags verify='+verify);
   probes++;
  }
 }
}
console.log(probes+' shift probes match the original translation semantics');
'''


def main():
    rng = random.Random(11)
    cases = []
    for op, sub in [('SHL', 4), ('SHL', 6), ('SHR', 5), ('SAR', 7)]:
        for w in (8, 16, 32):
            for form in ('imm', 'one'):
                for count in ([1] if form == 'one' else
                              [0, 1, 2, 7, 8, 15, 16, 31, 32, 33, 64 + 3]):
                    for dst in range(8):
                        pre = b'\x66' if w == 16 else b''
                        b = (0xd0 if form == 'one' else 0xc0) | (w != 8)
                        code = pre + bytes([b, 0xc0 | sub << 3 | dst]) + (
                            bytes([count & 0xff]) if form == 'imm' else b'')
                        cases.append(dict(op=op, width=w, dst=dst,
                                          count=count & 31, hex=code.hex(),
                                          raw=code))
    with tempfile.TemporaryDirectory(prefix='direct-shift-') as tmp:
        tmp = Path(tmp)
        (tmp/'t.c').write_text(C)
        subprocess.run([*shlex.split(os.environ.get('CC', 'cc')), '-O1',
                        '-g', '-fsanitize=address,undefined',
                        '-fno-sanitize-recover=all', '-I' + str(ROOT),
                        '-I' + str(ROOT/'include'), str(tmp/'t.c'), '-o',
                        str(tmp/'t')], check=True)
        lines = [f'{len(c["raw"])} ' + ' '.join(f'{x:02x}' for x in c['raw'])
                 for c in cases]
        out = subprocess.run([str(tmp/'t')], input=(f'{len(lines)}\n' +
                             '\n'.join(lines) + '\n').encode(),
                             capture_output=True, check=True).stdout
        shl = list(struct.unpack_from('<3I', out, 0))
        sar = list(struct.unpack_from('<3I', out, 12))
        pos, js, n = 24, [], 0
        for c in cases:
            (ok,) = struct.unpack_from('<I', out, pos)
            pos += 4
            assert ok, c['hex']
            paths = []
            for verify in range(2):
                (size,) = struct.unpack_from('<I', out, pos)
                body = out[pos + 4:pos + 4 + size]
                pos += 4 + size
                p = tmp/f'm{n}.wasm'
                n += 1
                p.write_bytes(D.module(body))
                paths.append(str(p))
            if c['count'] == 0:
                continue        # decoded as NOP: covered by the decode check
            js.append(dict(op=c['op'], width=c['width'], dst=c['dst'],
                           count=c['count'], hex=c['hex'], code=paths,
                           tests=[dict(regs=[rng.getrandbits(32)
                                             for _ in range(8)])
                                  for _ in range(4)]))
        assert pos == len(out)
        flags = tmp/'flags.wasm'
        (tmp/'flags.c').write_text(D.FLAGS_C)
        clang = os.environ.get('WASM_CLANG', str(ROOT.parent /
                               'emsdk/upstream/bin/clang'))
        subprocess.run([clang, '--target=wasm32', '-O2', '-nostdlib',
                        '-Wl,--no-entry', '-Wl,--export=helper_cc_compute_all',
                        '-I' + str(ROOT), str(tmp/'flags.c'), '-o',
                        str(flags)], check=True)
        (tmp/'c.json').write_text(json.dumps(dict(cases=js, shl=shl, sar=sar,
                                                  flagsWasm=str(flags))))
        (tmp/'run.cjs').write_text(JS)
        res = subprocess.run([*shlex.split(os.environ.get('NODE', 'node')),
                              str(tmp/'run.cjs'), str(tmp/'c.json')],
                             check=True, capture_output=True, text=True)
        print(res.stdout.strip())
        print(f'PASS: {len(cases)} shift encodings decoded '
              f'({len(cases) - len(js)} zero-count NOPs)')


if __name__ == '__main__':
    main()
