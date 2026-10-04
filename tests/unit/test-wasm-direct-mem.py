#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Direct x86 memory stage: decoder forms and emitted Wasm, with mock hooks.

Oracle: a load form with a TLB hit must leave exactly the registers and lazy
flags of the same instruction in register form (already checked against
native IA32 by test-wasm-direct.py) with the loaded value in a spare register.
Stores must write exactly their bytes; PUSH/POP are checked directly. A TLB
miss (flag bits in the tag, or an access crossing into an unmapped page) must
take slow() and leave host memory untouched; verify mode must call only
shadow(). Mock hooks: not the backend integration or a real softmmu.
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

# fixed mock layout (env at 4096, as in test-wasm-direct.py)
ENV = 4096
MASK_OFS, TABLE_OFS = 256, 260        # env-relative CPUTLBDescFast fields
TABLE, ENTRY_BITS, ENTRIES = 16384, 4, 256
HOST = 32768                          # host copy of the mapped guest page
PAGE = 0x00123000                     # mapped guest page
SLOW_FLAG, SLOW_LOAD, SLOW_STORE, SLOW_ADDR = 12000, 12004, 12008, 12012
SHADOW_FLAG, SHADOW_LOAD, SHADOW_STORE, SHADOW_ADDR = 12020, 12024, 12028, 12032

C = D.C[:D.C.index('int main(void)')] + r'''
static bool mock_resolve(XwdEmitter *e, unsigned insn, unsigned access,
                         bool store, unsigned width, XwdMemAccess *a)
{
 (void)e; (void)access;
 a->insn = insn; a->store = store; a->width = width;
 a->oi = 0; a->mask_ofs = ''' + str(MASK_OFS) + r'''; a->table_ofs = ''' + \
    str(TABLE_OFS) + r''';
 a->cmp_ofs = store ? 4 : 0; a->addend_ofs = 8;
 a->index_shift = 12 - ''' + str(ENTRY_BITS) + r''';
 a->fold_shift = 12; a->fold_mask = 0xff000; a->page_mask = 0xfffff000u;
 a->a_mask = 0; a->s_mask = width / 8 - 1;
 return true;
}
static bool mock_boundary(XwdEmitter *e, const XwdMemAccess *a)
{
 (void)a; xwd_byte(e, 0x01); /* nop */ return true;
}
static void mark(XwdEmitter *e, uint32_t flag, uint32_t load, uint32_t store,
                 uint32_t addr, const XwdMemAccess *a, unsigned al,
                 unsigned vl)
{
 xwd_const(e, flag); xwd_const(e, a->index + 1); xwd_memop(e, 0x36, 2, 0);
 xwd_const(e, addr); xwd_local(e, 0x20, al); xwd_memop(e, 0x36, 2, 0);
 if (a->store) {
  xwd_const(e, store); xwd_local(e, 0x20, vl); xwd_memop(e, 0x36, 2, 0);
 } else {
  xwd_const(e, load); xwd_memop(e, 0x28, 2, 0); xwd_local(e, 0x21, vl);
 }
}
static bool mock_slow(XwdEmitter *e, const XwdMemAccess *a, unsigned al,
                      unsigned vl)
{
 mark(e, ''' + f'{SLOW_FLAG}, {SLOW_LOAD}, {SLOW_STORE}, {SLOW_ADDR}' + r''',
      a, al, vl);
 return true;
}
static bool mock_shadow(XwdEmitter *e, const XwdMemAccess *a, unsigned al,
                        unsigned vl)
{
 mark(e, ''' + f'{SHADOW_FLAG}, {SHADOW_LOAD}, {SHADOW_STORE}, {SHADOW_ADDR}' + \
    r''', a, al, vl);
 return true;
}
static const XwdMemHooks hooks = { .resolve = mock_resolve,
 .resume_boundary = mock_boundary, .slow = mock_slow, .shadow = mock_shadow };
static void out_code(const XwdCode *code) {
 assert(fwrite(&code->size,4,1,stdout)==1);
 assert(fwrite(code->bytes,1,code->size,stdout)==code->size);
}
int main(void) {
 check_capture();
 unsigned cases; assert(scanf("%u",&cases)==1);
 for(unsigned i=0;i<cases;i++) {
  XwdCapture cap={.version=XWD_VERSION,.valid=true,.pc=0x40000000,
                  .count=1,.fallthrough=true};
  unsigned size, mem; assert(scanf("%u%u",&size,&mem)==2);
  cap.pcrel = mem == 2;   /* 2: PC-relative TB (EIP from env at run time) */
  cap.size=size; cap.end[0]=size;
  for(unsigned j=0;j<size;j++){unsigned v;assert(scanf("%x",&v)==1);cap.bytes[j]=v;}
  XwdPlan p, q;
  uint32_t ok=xwd_decode_ex(&cap,&p,mem!=0), plain=xwd_decode(&cap,&q);
  /* register-only decoding never admits a memory form */
  assert(!ok || !p.accesses || !plain);
  uint32_t info[5]={ok,ok?p.accesses:0,ok?p.insn[0].mem_store:0,
                    ok?p.insn[0].mem_width:0,ok?p.insn[0].op:0};
  assert(fwrite(info,sizeof(info),1,stdout)==1);
  if(!ok)continue;
  for(int verify=0;verify<2;verify++){
   XwdCode code;
   assert(xwd_emit_ex(&p,&layout,verify,8192,0,&hooks,&code));
   out_code(&code);
  }
  /* without hooks a memory plan must decline */
  if(p.accesses){XwdCode code;assert(!xwd_emit(&p,&layout,0,8192,0,&code));}
 }
}
'''

JS = r'''
const fs=require('fs'), assert=require('assert');
const d=JSON.parse(fs.readFileSync(process.argv[2]));
const memory=new WebAssembly.Memory({initial:1});
const bytes=new Uint8Array(memory.buffer), v=new DataView(memory.buffer);
const flags=new WebAssembly.Instance(new WebAssembly.Module(
 fs.readFileSync(d.flagsWasm)),{}).exports.helper_cc_compute_all;
const inst=(p)=>new WebAssembly.Instance(new WebAssembly.Module(
 fs.readFileSync(p)),{env:{memory},helper:{cc:flags}});
const M=d.m, mask=0x8d5;
function tlb(read, write){
 const idx=((((M.PAGE^((M.PAGE>>>12)&0xff000))>>>(12-M.EB))>>>0)&((M.N-1)<<M.EB));
 const e=M.TABLE+idx;
 v.setUint32(e,read,true); v.setUint32(e+4,write,true);
 v.setUint32(e+8,(M.HOST-M.PAGE)>>>0,true);
}
function setup(t, regs){
 bytes.fill(0x5a); v.setUint32(128,M.ENV,true);
 for(let i=0;i<8;i++)v.setUint32(M.ENV+4*i,regs[i],true);
 v.setUint32(M.ENV+32,t.eip||0x40000000,true);
 for(let i=0;i<4;i++)v.setUint32(M.ENV+40+4*i,t.lazy[i],true);
 v.setUint32(M.ENV+M.MASK,(M.N-1)<<M.EB,true);
 v.setUint32(M.ENV+M.TABLEP,M.TABLE,true);
 for(let i=0;i<4096;i++)bytes[M.HOST+i]=t.page[i];
 v.setUint32(M.SLOW+4,t.slow,true); v.setUint32(M.SHADOW+4,t.slow,true);
}
function state(out){   // regs, cc value, eip
 const r=[];for(let i=0;i<8;i++)r.push(v.getUint32(out+4*i,true));
 const c=out===8192?out+32:out+40;
 const cc=flags(v.getUint32(c,true),v.getUint32(c+4,true),
                v.getUint32(c+8,true),v.getUint32(c+12,true))&mask;
 return {r,cc,eip:v.getUint32(out===8192?out+48:out+32,true)};
}
let probes=0;
for(const c of d.cases){
 const mm=c.mem.map(inst), rm=c.reg?c.reg.map(inst):null;
 for(const t of c.tests){
  for(const mode of ['hit','miss','verify']){
   const verify=mode==='verify'?1:0;
   setup(t,t.regs);
   if(mode==='miss')tlb((M.PAGE|0x20)>>>0,(M.PAGE|0x20)>>>0);
   else tlb(M.PAGE,M.PAGE);
   const before=bytes.slice();
   assert.equal(mm[verify].exports.start(128),0,c.hex);
   const got=state(verify?8192:M.ENV);
   const fast=mode==='hit' && !t.cross;
   if(c.kind==='pushm'){
    // two accesses: load [m] (old ESP), store to [ESP-4]; ESP -= 4
    const r=t.regs.slice();r[4]=(r[4]-4)>>>0;
    assert.deepEqual(got.r,r,'pushm regs '+mode+' '+c.hex);
    const so=t.regs[4]-4-M.PAGE, after=bytes.slice();
    let src=0;for(let i=0;i<4;i++)src|=t.page[t.addr-M.PAGE+i]<<(8*i);
    if(fast){
     let st=0;for(let i=0;i<4;i++)st|=after[M.HOST+so+i]<<(8*i);
     assert.equal(st>>>0,src>>>0,'pushm stored '+c.hex);
     for(let i=0;i<4096;i++)if(i<so||i>=so+4)
      assert.equal(after[M.HOST+i],before[M.HOST+i],'pushm page '+c.hex);
    }else{
     const base=verify?M.SHADOW:M.SLOW;
     assert.equal(v.getUint32(base,true),2,'both accesses via hook '+c.hex);
     assert.equal(v.getUint32(base+8,true)>>>0,t.slow>>>0,'pushm hook store '+c.hex);
     assert.equal(v.getUint32(base+12,true),(t.regs[4]-4)>>>0,'pushm store addr');
     for(let i=0;i<4096;i++)assert.equal(after[M.HOST+i],before[M.HOST+i]);
    }
    assert.equal(got.eip,(0x40000000+c.len)>>>0);
    probes++;continue;
   }
   const slowRan=v.getUint32(M.SLOW,true), shadowRan=v.getUint32(M.SHADOW,true);
   assert.equal(slowRan, fast||verify?0x5a5a5a5a:1, 'slow '+mode+' '+c.hex);
   assert.equal(shadowRan, verify?1:0x5a5a5a5a, 'shadow '+mode+' '+c.hex);
   const hookAddr=verify?v.getUint32(M.SHADOW+12,true):v.getUint32(M.SLOW+12,true);
   if(!fast)assert.equal(hookAddr,t.addr,'hook address '+c.hex);
   // the value the access produced (or the one the hook supplied)
   const off=t.addr-M.PAGE;
   const w=c.width/8, lo=(x)=>w===4?x>>>0:(x&((1<<(8*w))-1))>>>0;
   let loaded=0;
   for(let i=0;i<w;i++)loaded|=t.page[off+i]<<(8*i);
   loaded=fast?lo(loaded):lo(t.slow);
   // host memory: only a fast store may change the page, exactly its bytes
   const after=bytes.slice();
   for(let i=0;i<4096;i++){
    const inStore=fast&&c.store&&i>=off&&i<off+w;
    if(!inStore)assert.equal(after[M.HOST+i],before[M.HOST+i],'page '+c.hex);
   }
   if(c.store){
    if(fast){
     let st=0;for(let i=0;i<w;i++)st|=after[M.HOST+off+i]<<(8*i);
     assert.equal(lo(st),lo(t.value),'stored '+c.hex);
    }else{
     const rec=v.getUint32((verify?M.SHADOW:M.SLOW)+8,true);
     assert.equal(lo(rec),lo(t.value),'hook store value '+c.hex);
    }
   }
   if(c.kind==='push'||c.kind==='call'){
    const r=t.regs.slice();r[4]=(r[4]-4)>>>0;
    assert.deepEqual(got.r,r,'push regs '+c.hex);
   }else if(c.kind==='pop'){
    const r=t.regs.slice();r[4]=(r[4]+4)>>>0;r[t.dst]=loaded;
    assert.deepEqual(got.r,r,'pop regs '+c.hex);
   }else if(c.store){
    assert.deepEqual(got.r,t.regs,'store leaves registers '+c.hex);
   }else{
    // register form with the loaded value in the spare register X
    const regs=t.regs.slice();regs[t.x]=loaded;
    setup(t,regs);tlb(M.PAGE,M.PAGE);
    assert.equal(rm[verify].exports.start(128),0);
    const want=state(verify?8192:M.ENV);
    for(let i=0;i<8;i++){
     if(i===t.x)continue;
     assert.equal(got.r[i],want.r[i],'GPR '+i+' '+mode+' '+c.hex);
    }
    assert.equal(got.cc,want.cc,'flags '+mode+' '+c.hex);
   }
   assert.equal(got.eip,((t.eip||0x40000000)+c.len+(c.rel||0))>>>0,'eip '+c.hex);
   probes++;
  }
 }
}
console.log(probes+' memory-form probes (hit/miss/verify) passed');
'''


def modrm_mem(reg, base, index, scale, disp):
    """ModRM (+SIB, disp32) for [base + index*scale + disp]."""
    if index is None and base != 4:
        return bytes([0x80 | reg << 3 | base]) + struct.pack('<i', disp)
    sib = scale << 6 | (4 if index is None else index) << 3 | base
    return bytes([0x84 | reg << 3, sib]) + struct.pack('<i', disp)


ALU = ['ADD', 'OR', 'ADC', 'SBB', 'AND', 'SUB', 'XOR', 'CMP']


def forms():
    """(name, kind, width, mem_bytes(reg, mem), reg_bytes(reg, x), store,
    immediate)"""
    out = []
    for w, op8, op32 in [(32, None, 0x8b), (8, 0x8a, None)]:
        b = op32 or op8
        out.append((f'MOV r{w},[m]', 'load', w,
                    lambda r, m, b=b: bytes([b]) + m,
                    lambda r, x, b=b: bytes([b, 0xc0 | r << 3 | x]), None))
    out.append(('MOV r16,[m]', 'load', 16,
                lambda r, m: b'\x66\x8b' + m,
                lambda r, x: bytes([0x66, 0x8b, 0xc0 | r << 3 | x]), None))
    for op, w in [(0xb6, 8), (0xb7, 16), (0xbe, 8), (0xbf, 16)]:
        out.append((f'MOV{"SX" if op & 8 else "ZX"} r32,[m{w}]', 'load', w,
                    lambda r, m, op=op: bytes([0x0f, op]) + m,
                    lambda r, x, op=op: bytes([0x0f, op, 0xc0 | r << 3 | x]),
                    None))
    for g in range(8):
        for w, base in [(32, 3), (8, 2)]:
            b = g << 3 | base
            out.append((f'{ALU[g]} r{w},[m]', 'load', w,
                        lambda r, m, b=b: bytes([b]) + m,
                        lambda r, x, b=b: bytes([b, 0xc0 | r << 3 | x]),
                        None))
    for w, b in [(32, 0x39), (8, 0x38), (32, 0x85), (8, 0x84)]:
        out.append((f'{"CMP" if b < 0x80 else "TEST"} [m{w}],r', 'cmp', w,
                    lambda r, m, b=b: bytes([b]) + m,
                    lambda r, x, b=b: bytes([b, 0xc0 | r << 3 | x]), None))
    for w, b, imm in [(32, 0x83, 1), (32, 0x81, 4), (8, 0x80, 1)]:
        out.append((f'CMP [m{w}],imm{imm * 8}', 'cmpimm', w,
                    lambda r, m, b=b: bytes([b]) + m,
                    lambda r, x, b=b: bytes([b, 0xf8 | x]), imm))
    for w, b, imm in [(32, 0xf7, 4), (8, 0xf6, 1)]:
        out.append((f'TEST [m{w}],imm', 'cmpimm', w,
                    lambda r, m, b=b: bytes([b]) + m,
                    lambda r, x, b=b: bytes([b, 0xc0 | x]), imm))
    for w, pre, b in [(32, b'', 0x89), (8, b'', 0x88), (16, b'\x66', 0x89)]:
        out.append((f'MOV [m{w}],r', 'store', w,
                    lambda r, m, b=b, pre=pre: pre + bytes([b]) + m,
                    None, None))
    for w, pre, b, imm in [(32, b'', 0xc7, 4), (8, b'', 0xc6, 1),
                           (16, b'\x66', 0xc7, 2)]:
        out.append((f'MOV [m{w}],imm', 'storeimm', w,
                    lambda r, m, b=b, pre=pre: pre + bytes([b]) + m,
                    None, imm))
    return out


DECLINE = [
    '0118',          # ADD [eax], ebx (read-modify-write)
    '830001',        # ADD dword [eax], 1
    'ff00',          # INC dword [eax]
    '0f4400',        # CMOVZ eax, [eax]
    '0f9400',        # SETZ [eax]
    '648b00',        # MOV eax, fs:[eax]
    '678b00',        # MOV eax, [bx+si] (address size)
    '6650',          # PUSH ax
    '8700',          # XCHG [eax], eax
    'f600ff',        # TEST... ok (allowed) -> replaced below
]
DECLINE = DECLINE[:-1] + ['f61000']   # NOT byte [eax]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tests', type=int, default=24)
    parser.add_argument('--seed', type=int, default=7)
    args = parser.parse_args()
    rng = random.Random(args.seed)
    cases = []          # (hex bytes, allow_memory)
    meta = []
    for name, kind, w, memf, regf, imm in forms():
        for t in range(args.tests):
            # registers: dst r, spare x (low byte reg for 8-bit operands),
            # base b, optional index i; all distinct
            x = rng.choice([0, 1, 2, 3])
            pool = [n for n in range(8) if n not in (x, 4)]
            r, b = rng.sample(pool, 2)
            if w == 8 and (r & 3) == x:
                # AH..BH alias the low registers: keep the spare distinct
                r = rng.choice([n for n in range(8) if n != b and n != 4 and
                                (n & 3) != x and n != x])
            index = rng.choice([None, *[n for n in pool if n not in (r, b)]])
            scale = rng.randrange(4) if index is not None else 0
            # the access lands inside the mapped page (sometimes crossing it)
            cross = rng.random() < 0.15 and w > 8
            off = (4096 - w // 8 + 1) if cross else rng.randrange(4096 - 4)
            addr = PAGE + off
            regs = [rng.getrandbits(32) for _ in range(8)]
            regs[4] = PAGE + 0x800 + 4 * rng.randrange(64)
            iv = rng.getrandbits(32) & 0x3ff
            if index is not None:
                regs[index] = iv
            disp = rng.randrange(-0x400, 0x400)
            regs[b] = (addr - disp - (iv << scale if index is not None
                                      else 0)) & 0xffffffff
            m = modrm_mem(r if kind not in ('cmpimm', 'storeimm') else
                          (7 if name.startswith('CMP') else 0),
                          b, index, scale, disp)
            immv = rng.getrandbits(8 * imm) if imm else 0
            immb = immv.to_bytes(imm, 'little') if imm else b''
            mem = memf(r, m) + immb
            reg = regf(r, x) + immb if regf else None
            if kind in ('store', 'storeimm'):
                value = immv if imm else (
                    (regs[r & 3] >> (8 if r >= 4 else 0)) if w == 8
                    else regs[r])
                if imm == 1 and w == 32:
                    value = (immv ^ 0x80) - 0x80
                value &= (1 << w) - 1
            else:
                value = 0
            meta.append(dict(name=name, kind=kind, width=w, mem=mem.hex(),
                             reg=reg.hex() if reg else None, regs=regs,
                             addr=addr, x=x, dst=r, cross=cross,
                             store=kind in ('store', 'storeimm'), value=value))
    # PUSH/POP r32
    for t in range(args.tests * 2):
        n = rng.randrange(8)
        push = t % 2 == 0
        regs = [rng.getrandbits(32) for _ in range(8)]
        regs[4] = PAGE + 0x800 + 4 * rng.randrange(64)
        addr = (regs[4] - 4) if push else regs[4]
        value = regs[n] if push else 0
        if push and n == 4:
            value = regs[4]
        meta.append(dict(name='PUSH' if push else 'POP',
                         kind='push' if push else 'pop', width=32,
                         mem=bytes([(0x50 if push else 0x58) + n]).hex(),
                         reg=None, regs=regs, addr=addr, x=0, dst=n,
                         cross=False, store=push, value=value))
    # PUSH [m]: [base + disp], source inside the page, stack elsewhere in it
    for t in range(args.tests * 2):
        regs = [rng.getrandbits(32) for _ in range(8)]
        regs[4] = PAGE + 0xc00 + 4 * rng.randrange(64)
        b = rng.choice([0, 1, 2, 3, 5, 6, 7])
        addr = PAGE + 4 * rng.randrange(0x200)
        disp = rng.randrange(-0x80, 0x80)
        regs[b] = (addr - disp) & 0xffffffff
        code = bytes([0xff]) + modrm_mem(6, b, None, 0, disp)
        meta.append(dict(name='PUSH [m]', kind='pushm', width=32,
                         mem=code.hex(), reg=None, regs=regs, addr=addr,
                         x=0, dst=0, cross=False, store=False, value=0))
    # PUSH imm8/imm32 and CALL rel32 (return address = pc + length)
    for t in range(args.tests * 3):
        regs = [rng.getrandbits(32) for _ in range(8)]
        regs[4] = PAGE + 0x800 + 4 * rng.randrange(64)
        kind = ('push8', 'push32', 'call')[t % 3]
        if kind == 'push8':
            iv = rng.getrandbits(8)
            code, value = bytes([0x6a, iv]), ((iv ^ 0x80) - 0x80) & 0xffffffff
            rel = 0
        elif kind == 'push32':
            iv = rng.getrandbits(32)
            code, value, rel = bytes([0x68]) + iv.to_bytes(4, 'little'), iv, 0
        else:
            rel = rng.randrange(-0x10000, 0x10000)
            code = bytes([0xe8]) + struct.pack('<i', rel)
            value = (0x50000000 if t % 2 == 1 else 0x40000000) + 5
        meta.append(dict(name=kind, kind='call' if kind == 'call' else 'push',
                         pcrel=kind == 'call' and t % 2 == 1,
                         width=32, mem=code.hex(), reg=None, regs=regs,
                         addr=regs[4] - 4, x=0, dst=0, cross=False,
                         store=True, value=value, rel=rel))
    # MOV AL/EAX,[moffs] and [moffs],AL/EAX (+66: AX)
    for t in range(args.tests * 2):
        for pre, b, w in [(b'', 0xa1, 32), (b'', 0xa0, 8), (b'\x66', 0xa1, 16),
                          (b'', 0xa3, 32), (b'', 0xa2, 8), (b'\x66', 0xa3, 16)]:
            regs = [rng.getrandbits(32) for _ in range(8)]
            regs[4] = PAGE + 0x800
            cross = rng.random() < 0.15 and w > 8
            off = (4096 - w // 8 + 1) if cross else rng.randrange(4096 - 4)
            addr = PAGE + off
            code = pre + bytes([b]) + addr.to_bytes(4, 'little')
            store = b >= 0xa2
            x = rng.choice([1, 2, 3])
            reg = None if store else (pre + bytes([0x8b if w > 8 else 0x8a,
                                                   0xc0 | x])).hex()
            meta.append(dict(name=f'MOV moffs{w}{" store" if store else ""}',
                             kind='store' if store else 'load', width=w,
                             mem=code.hex(), reg=reg, regs=regs, addr=addr,
                             x=x, dst=0, cross=cross, store=store,
                             value=regs[0] & ((1 << w) - 1)))
    for h in DECLINE:
        meta.append(dict(name='decline', kind='decline', mem=h))
    lines = []
    for item in meta:
        for code, allow in [(item['mem'], 2 if item.get('pcrel') else 1)] + (
                [(item['reg'], 0)] if item.get('reg') else []):
            bs = bytes.fromhex(code)
            lines.append(f'{len(bs)} {allow} ' + ' '.join(f'{x:02x}'
                                                          for x in bs))
    with tempfile.TemporaryDirectory(prefix='direct-mem-') as tmp:
        tmp = Path(tmp)
        (tmp/'t.c').write_text(C)
        subprocess.run([*shlex.split(os.environ.get('CC', 'cc')), '-O1',
                        '-g', '-fsanitize=address,undefined',
                        '-fno-sanitize-recover=all', '-I' + str(ROOT),
                        '-I' + str(ROOT/'include'), str(tmp/'t.c'), '-o',
                        str(tmp/'t')], check=True)
        res = subprocess.run([str(tmp/'t')], input=(f'{len(lines)}\n' +
                             '\n'.join(lines) + '\n').encode(),
                             capture_output=True)
        if res.returncode:
            raise SystemExit(res.stderr.decode()[-3000:])
        out = res.stdout
        pos = 0

        def read_code():
            nonlocal pos
            (n,) = struct.unpack_from('<I', out, pos)
            body = out[pos + 4:pos + 4 + n]
            pos += 4 + n
            return body

        js_cases = []
        mods = 0
        admitted = 0
        for item in meta:
            info = struct.unpack_from('<5I', out, pos)
            pos += 20
            ok, accesses, store, width, op = info
            paths = []
            if ok:
                for verify in range(2):
                    p = tmp/f'm{mods}.wasm'
                    mods += 1
                    p.write_bytes(D.module(read_code()))
                    paths.append(str(p))
            reg_paths = None
            if item.get('reg'):
                rinfo = struct.unpack_from('<5I', out, pos)
                pos += 20
                assert rinfo[0] and not rinfo[1], item['name']
                reg_paths = []
                for verify in range(2):
                    p = tmp/f'm{mods}.wasm'
                    mods += 1
                    p.write_bytes(D.module(read_code()))
                    reg_paths.append(str(p))
            if item['kind'] == 'decline':
                assert not ok, 'must decline: ' + item['mem']
                continue
            assert ok and accesses == (2 if item['kind'] == 'pushm' else 1), \
                item['name']
            assert store == item['store'] and width == item['width'], item
            admitted += 1
            page = [rng.getrandbits(8) for _ in range(4096)]
            slow = rng.getrandbits(32)
            js_cases.append(dict(
                hex=item['mem'], len=len(bytes.fromhex(item['mem'])),
                rel=item.get('rel', 0),
                width=item['width'], store=item['store'], kind=item['kind'],
                mem=paths, reg=reg_paths,
                tests=[dict(regs=item['regs'], addr=item['addr'], x=item['x'],
                            dst=item['dst'], cross=item['cross'],
                            value=item['value'], page=page, slow=slow,
                            eip=0x50000000 if item.get('pcrel') else None,
                            lazy=[0x89abcdef, 0x55, 0, 1])]))
        assert pos == len(out)
        flags_src = tmp/'flags.c'
        flags_src.write_text(D.FLAGS_C)
        flags = tmp/'flags.wasm'
        clang = os.environ.get('WASM_CLANG', str(ROOT.parent /
                               'emsdk/upstream/bin/clang'))
        subprocess.run([clang, '--target=wasm32', '-O2', '-nostdlib',
                        '-Wl,--no-entry', '-Wl,--export=helper_cc_compute_all',
                        '-I' + str(ROOT), str(flags_src), '-o', str(flags)],
                       check=True)
        m = dict(ENV=ENV, MASK=MASK_OFS, TABLEP=TABLE_OFS, TABLE=TABLE,
                 EB=ENTRY_BITS, N=ENTRIES, HOST=HOST, PAGE=PAGE,
                 SLOW=SLOW_FLAG, SHADOW=SHADOW_FLAG)
        (tmp/'cases.json').write_text(json.dumps(dict(
            m=m, cases=js_cases, flagsWasm=str(flags))))
        (tmp/'run.cjs').write_text(JS)
        res = subprocess.run([*shlex.split(os.environ.get('NODE', 'node')),
                              str(tmp/'run.cjs'), str(tmp/'cases.json')],
                             check=True, capture_output=True, text=True)
        print(res.stdout.strip())
        print(f'PASS: {admitted} memory forms admitted, {len(DECLINE)} '
              f'declined; register-only decode rejects all memory forms')


if __name__ == '__main__':
    main()
