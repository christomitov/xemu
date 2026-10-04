#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Bounded direct decoder/emitter tests, with native IA32 arithmetic oracle.

--wasm executes emitted Wasm. This is not the full TB wrapper/TCG differential
integration gate: use DIRECT_X86=2 with DIRECT_FLAGS=1 in the actual emulator.
"""
import argparse
import json
import os
from pathlib import Path
import random
import re
import shlex
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
CPU = (ROOT / 'target/i386/cpu.h').read_text()
end = CPU.index('} CCOp;') + len('} CCOp;')
start = CPU.rfind('typedef enum', 0, end)
CC = CPU[start:end]
CC_NAMES = ['CC_OP_EFLAGS'] + [f'CC_OP_{op}{w}' for op in
    ['ADD', 'SUB', 'LOGIC', 'ADC', 'SBB', 'INC', 'DEC', 'SHL', 'SAR', 'MUL']
    for w in ['B', 'W', 'L']]


def c_function(text, name):
    match = re.search(r'^(?:static )?[^\n]*\b' + name + r'\(', text, re.M)
    start = match.start()
    return text[start:text.index('\n}\n', start)+3]


def cc_source():
    helpers = (ROOT / 'target/i386/tcg/cc_helper.c').read_text()
    common = (ROOT / 'target/i386/tcg/helper-tcg.h').read_text()
    macros = '\n'.join(re.findall(r'^#define CC_[CPAZSO] +.*', CPU, re.M))
    macros += '\n' + CPU[CPU.index('#define MAJ_INV1'):CPU.index(
        '#endif /* I386_CPU_H */')]
    prefix = ('#include <stdint.h>\n'
        'typedef uint32_t target_ulong; typedef int32_t target_long;\n'
        '#define HOST_LONG_BITS 32\n'
        '#define glue1(a,b) a##b\n#define glue(a,b) glue1(a,b)\n'
        '#define parity8(x) __builtin_parity((unsigned)(uint8_t)(x))\n')
    result = (prefix + '#pragma GCC diagnostic push\n'
              '#pragma GCC diagnostic ignored "-Wunused-function"\n'
              '#pragma GCC diagnostic ignored "-Wunused-parameter"\n' +
              CC + '\n' + macros + '\n')
    result += c_function(common, 'lshift') + c_function(common, 'compute_pf')
    for shift in range(3):
        result += (f'#define SHIFT {shift}\n'
                   '#include "target/i386/tcg/cc_helper_template.h.inc"\n'
                   '#undef SHIFT\n')
    for name in ['compute_all_adcx', 'compute_all_adox', 'compute_all_adcox',
                 'helper_cc_compute_all']:
        result += c_function(helpers, name)
    return result + '#pragma GCC diagnostic pop\n'


FLAGS_C = cc_source()
FRONT = (ROOT / 'target/i386/tcg/wasm-direct.c.inc').read_text()

def frontend_function(name):
    start = FRONT.index('static void ' + name + '(')
    end = FRONT.index('\n}\n', start) + 3
    return FRONT[start:end]

CAPTURE = r'''
enum { DISAS_TOO_MANY=1, DISAS_NORETURN=2 };
typedef struct {
 XwdTranslation *wasm_direct;
 uint64_t pc;
 struct { bool plugin_enabled; unsigned num_insns,is_jmp; } base;
} DisasContext;
''' + ''.join(frontend_function(n) for n in [
    'x86_wasm_direct_bytes', 'x86_wasm_direct_insn',
    'x86_wasm_direct_jump', 'x86_wasm_direct_stop']) + r'''
static void check_capture(void) {
 XwdTranslation t={.capture={.version=XWD_VERSION,.pc=0xfffffffe,.valid=true}};
 DisasContext s={.wasm_direct=&t,.pc=0x100000000ULL};
 x86_wasm_direct_bytes(&s,0x3412,2);
 assert(t.capture.size==2 && t.capture.bytes[0]==0x12 &&
        t.capture.bytes[1]==0x34);
 x86_wasm_direct_insn(&s);
 assert(t.capture.count==1 && t.capture.end[0]==2);
 s.pc+=4;x86_wasm_direct_bytes(&s,0x12345678,4);
 x86_wasm_direct_insn(&s);
 assert(t.capture.count==2 && t.capture.end[1]==6);
 s.base.num_insns=2;s.base.is_jmp=DISAS_TOO_MANY;
 x86_wasm_direct_stop(&s);assert(t.capture.valid && t.capture.fallthrough);
 x86_wasm_direct_jump(&s,0,0xffffffff);
 x86_wasm_direct_jump(&s,1,0x1234);
 assert(t.capture.jump_valid==3 && t.capture.jump_pc[1]==0x1234);
 x86_wasm_direct_jump(&s,1,0x4321);assert(!t.capture.valid);
 t.capture.valid=true;
 s.base.plugin_enabled=true;x86_wasm_direct_stop(&s);assert(!t.capture.valid);
 /* Missing/duplicated bytes never become a speculative reread. */
 t.capture.valid=true;s.pc+=2;x86_wasm_direct_bytes(&s,0x90,1);
 assert(!t.capture.valid && t.capture.size==6);
 t.capture.valid=true;x86_wasm_direct_bytes(&s,0,16);
 assert(!t.capture.valid);
 t.capture.valid=true;t.capture.size=XWD_MAX_BYTES;
 s.pc=(uint64_t)t.capture.pc+XWD_MAX_BYTES+1;
 x86_wasm_direct_bytes(&s,0x90,1);assert(!t.capture.valid);
 t.capture.valid=true;t.capture.count=XWD_MAX_INSNS;
 x86_wasm_direct_insn(&s);assert(!t.capture.valid);
 s.wasm_direct=NULL;x86_wasm_direct_bytes(&s,1,1);
 x86_wasm_direct_insn(&s);x86_wasm_direct_stop(&s);
}
'''
C = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "qemu/xemu-wasm-direct.h"
''' + FLAGS_C + CAPTURE + r'''
#include "tcg/wasm32-direct.c.inc"
static const XwdLayout layout={.regs=0,.eip=32,.cc_dst=40,.cc_src=44,
 .cc_src2=48,.cc_op=52,.can_do_io=-4,
 .cc_add={CC_OP_ADDB,CC_OP_ADDW,CC_OP_ADDL},
 .cc_sub={CC_OP_SUBB,CC_OP_SUBW,CC_OP_SUBL},
 .cc_logic={CC_OP_LOGICB,CC_OP_LOGICW,CC_OP_LOGICL},
 .cc_adc={CC_OP_ADCB,CC_OP_ADCW,CC_OP_ADCL},
 .cc_sbb={CC_OP_SBBB,CC_OP_SBBW,CC_OP_SBBL},
 .cc_inc={CC_OP_INCB,CC_OP_INCW,CC_OP_INCL},
 .cc_dec={CC_OP_DECB,CC_OP_DECW,CC_OP_DECL},.cc_eflags=CC_OP_EFLAGS,
 .phase_ptr=9000,.phase_cc=0x1111,.phase_generated=0x2222};
int main(void) {
 check_capture();
 unsigned cases; assert(scanf("%u",&cases)==1);
 uint32_t constants[]={''' + ','.join(CC_NAMES) + r'''};
 assert(fwrite(constants,sizeof(constants),1,stdout)==1);
 /* Exhaustive input status flags, then varied valid incoming lazy tuples. */
 const unsigned bits[]={1,4,16,64,128,2048};
 for(unsigned k=0;k<128;k++) {
  uint32_t state[5]={0x89abcdef,0,0,CC_OP_EFLAGS,0};
  if(k<64) {
   for(unsigned n=0;n<6;n++)if(k&(1u<<n))state[1]|=bits[n];
  }else{
   unsigned index=1+(k-64)%(sizeof(constants)/4-1);
   state[3]=constants[index];state[2]=k&1;
   state[0]=k*0x1234567u;state[1]=~(k*0x45678u);
   if(index>=16 && index<=21)state[1]=k&1; /* INC/DEC: saved carry */
  }
  state[4]=helper_cc_compute_all(state[0],state[1],state[2],state[3]);
  assert(fwrite(state,sizeof(state),1,stdout)==1);
 }
 for(unsigned i=0;i<cases;i++) {
  XwdCapture cap={.version=XWD_VERSION,.valid=true,.pc=0x40000000};
  unsigned size,count,fallthrough,relative;
  assert(scanf("%u%u%u%u",&size,&count,&fallthrough,&relative)==4);
  assert(size<=XWD_MAX_BYTES && count<=XWD_MAX_INSNS);
  cap.size=size;cap.count=count;cap.fallthrough=fallthrough;cap.pcrel=relative;
  for(unsigned j=0;j<count;j++) {
   unsigned v; assert(scanf("%u",&v)==1); cap.end[j]=v;
  }
  for(unsigned j=0;j<size;j++) {
   unsigned v; assert(scanf("%x",&v)==1); cap.bytes[j]=v;
  }
  XwdPlan p;
  uint32_t ok=xwd_decode(&cap,&p);
  assert(fwrite(&ok,4,1,stdout)==1);
  if(!ok)continue;
  assert(fwrite(&p.delta,4,1,stdout)==1);
  for(int verify=0;verify<2;verify++) {
   XwdCode code;
   assert(xwd_emit(&p,&layout,verify,8192,0,&code));
   assert(fwrite(&code.size,4,1,stdout)==1);
   assert(fwrite(code.bytes,1,code.size,stdout)==code.size);
  }
 }
}
'''


def uleb(n):
    result = bytearray()
    while True:
        b = n & 127
        n >>= 7
        result.append(b | (128 if n else 0))
        if not n:
            return result


def section(i, data):
    return bytes([i]) + uleb(len(data)) + data


def module(body):
    return (b'\0asm\x01\0\0\0' +
            section(1, b'\x02\x60\x01\x7f\x01\x7f'
                       b'\x60\x04\x7f\x7f\x7f\x7f\x01\x7f') +
            section(2, b'\x02\x03env\x06memory\x02\x00\x01'
                       b'\x06helper\x02cc\x00\x01') +
            section(3, b'\x01\x00') +
            section(7, b'\x01\x05start\x00\x01') +
            section(10, b'\x01' + uleb(len(body)) + body))


JS = r'''
const fs=require('fs'), assert=require('assert');
const data=JSON.parse(fs.readFileSync(process.argv[2]));
const memory=new WebAssembly.Memory({initial:1});
const bytes=new Uint8Array(memory.buffer), v=new DataView(memory.buffer);
const mask=0x8d5;
// Actual QEMU lazy-CC helper, compiled to Wasm: no JS per-call wrapper.
const helper=new WebAssembly.Instance(new WebAssembly.Module(
 fs.readFileSync(data.flagsWasm)),{});
const flags=helper.exports.helper_cc_compute_all;
for(const s of data.states)assert.equal(flags(...s.slice(0,4))>>>0,s[4]);
let probes=0;
for(const item of data.cases){
 const mods=item.modules.map(p=>new WebAssembly.Instance(
  new WebAssembly.Module(fs.readFileSync(p)),{env:{memory},helper:{cc:flags}}));
 for(const test of item.tests){
  for(let verify=0;verify<2;verify++){
   bytes.fill(0x5a);v.setUint32(128,4096,true);
   for(let i=0;i<8;i++)v.setUint32(4096+4*i,test.regs[i],true);
   v.setUint32(4128,test.pc,true);
   for(let i=0;i<4;i++)v.setUint32(4136+4*i,test.lazy[i],true);
   const before=bytes.slice();
   assert.equal(mods[verify].exports.start(128),0);
   const out=verify?8192:4096;
   for(let i=0;i<8;i++){
    assert.equal(v.getUint32(out+4*i,true),test.want[i],
      'GPR '+i+' '+item.hex+' verify='+verify);
   }
   const d=verify?out+32:out+40;
   const got=flags(v.getUint32(d,true),v.getUint32(d+4,true),
                  v.getUint32(d+8,true),v.getUint32(d+12,true));
   assert.equal(got&mask,test.want[8]&mask,'CC '+item.hex+' verify='+verify);
   const delta=item.conditional && !test.want[9]?item.length:item.delta;
   assert.equal(v.getUint32(verify?out+48:out+32,true),
                ((item.relative?test.pc:0x40000000)+delta)>>>0,
                'selected PC '+item.hex+' verify='+verify);
   if(verify){
    assert.equal(v.getUint32(out+52,true),1);
    assert.equal((v.getUint32(out+56,true)>>1)&1,+item.conditional);
    bytes.set(before.subarray(out,out+60),out);
   }else{
    assert.equal(bytes[4092],1);
    bytes[4092]=before[4092];
    bytes.set(before.subarray(4096,4152),4096);
   }
   if(v.getUint32(9000,true)!==0x5a5a5a5a){
    assert.equal(v.getUint32(9000,true),0x2222,'restored generated scope');
    bytes.set(before.subarray(9000,9004),9000);
   }
   assert.deepEqual(bytes,before,'outside architectural/scratch/phase state');
   probes++;
  }
 }
}
console.log(probes+' direct probes match native IA32; shadow preserves guest');
'''


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--wasm', action='store_true')
    args = ap.parse_args()
    rng = random.Random(1933)
    cases = []
    def add(insns, valid=True, fall=True, relative=False, native=True,
            conditional=False, exhaustive=False):
        cases.append(dict(insns=insns, valid=valid, fall=fall,
                          relative=relative, native=native,
                          conditional=conditional, exhaustive=exhaustive))
    for width in [8, 16, 32]:
        pre = b'\x66' if width == 16 else b''
        for group in range(8):
            for direction in [0, 2]:
                for pair in [(0, 1), (4, 0), (7, 4), (3, 3)]:
                    op = (group << 3) | direction | (width != 8)
                    add([pre + bytes([op, 0xc0 | pair[0] << 3 | pair[1]])])
        for reg in range(8):
            immediate = (rng.getrandbits(width)).to_bytes(width//8, 'little')
            add([pre + bytes([(0xb0 if width == 8 else 0xb8)+reg])+immediate])
            add([pre + bytes([0xf6 if width == 8 else 0xf7, 0xd8 | reg])])
        for opcode in [0x88, 0x89, 0x8a, 0x8b, 0x86, 0x87]:
            add([pre + bytes([opcode, 0xe0])])
    for width in [8, 16, 32]:
        pre = b'\x66' if width == 16 else b''
        imm = (2**(width-1)+3).to_bytes(width//8, 'little')
        for group in range(8):
            add([pre + bytes([(group << 3) | 4 | (width != 8)]) + imm])
            add([pre + bytes([0x80 if width == 8 else 0x81,
                              0xc4 | (group << 3)]) + imm])
            if width != 8:
                add([pre + bytes([0x83, 0xc4 | (group << 3), 0x80])])
        add([pre + bytes([0xa8 if width == 8 else 0xa9]) + imm])
        add([pre + bytes([0xf6 if width == 8 else 0xf7, 0xc7]) + imm])
        add([pre + bytes([0xc6 if width == 8 else 0xc7, 0xc4]) + imm])
        add([pre + bytes([0x84 if width == 8 else 0x85, 0xe7])])
    for src in [0xb6, 0xb7, 0xbe, 0xbf]:
        for pre in [b'', b'\x66']:
            add([pre + bytes([0x0f, src, 0xc4])])
    for raw in ['8d4488ff', '8d042578563412', '8d04fd00000080',
                '8da42478563412', '668d4488ff', '90', '6690', '91', '6697']:
        add([bytes.fromhex(raw)])
    for width in [8, 16, 32]:
        pre = b'\x66' if width == 16 else b''
        for reg in range(8):
            for dec in [0, 1]:
                add([pre + bytes([0xfe if width == 8 else 0xff,
                                  0xc0 | dec << 3 | reg])])
                if width != 8:
                    add([pre + bytes([0x40 | dec << 3 | reg])])
    for op in [0xf8, 0xf9, 0xf5]:
        add([bytes([op])], exhaustive=True)
    for cond in range(16):
        add([bytes([0x0f, 0x90+cond, 0xc4])], exhaustive=True)
        add([bytes([0x0f, 0x40+cond, 0xc4])], exhaustive=True)
        add([bytes([0x66, 0x0f, 0x40+cond, 0xec])])
    # Carry and conditions through mixtures of preserving/updating operations.
    for _ in range(80):
        seq = [rng.choice(cases)['insns'][0] for _ in range(8)]
        seq += [b'\xf7\xd0', b'\x86\xe0', b'\x66\xb8\x00\x80']
        add(seq, relative=bool(rng.getrandbits(1)))
    for cond in range(16):
        add([bytes([0x70+cond, 0xfe])], fall=False, relative=True,
            conditional=True, exhaustive=True)
        add([bytes([0x0f, 0x80+cond, 0xff, 0xff, 0xff, 0x7f])],
            fall=False, conditional=True)
        add([b'\xf9', b'\x66\x83\xd8\xff', b'\xfe\xc4',
             bytes([0x70+cond, 0x80])], fall=False, relative=True,
            conditional=True)
    add([b'\xeb\xfe'], fall=False, relative=True, native=False)
    add([b'\xe9\x00\x00\x00\x80'], fall=False, native=False)
    for raw in ['8b00', '8900', '50', '58', 'c3', 'c9', 'e800000000',
                'f001c0', 'f390', '678d00', '64678b00', '666690', 'd9e8',
                '0f6fc0', '0f58c0', 'd1e0', '0fa2', '0f4400', '0f9400',
                'fe00', 'ffc0ff', 'ffc8ff', 'ffd0', 'ffe0', 'fff0',
                '66ebfe', '667400', '660f840000', '7400ff', '0f800000',
                'b80102', 'b80102030400', 'c7c801020304', '8dc0']:
        add([bytes.fromhex(raw)], valid=False, native=False)
    add([b'\x90'], valid=False, fall=False, native=False)
    add([b'\xeb\xfe', b'\x90'], valid=False, native=False)
    with tempfile.TemporaryDirectory(prefix='wasm-direct-') as td:
        tmp = Path(td)
        src = tmp/'emit.c'; src.write_text(C)
        exe = tmp/'emit'
        subprocess.run([*shlex.split(os.environ.get('CC', 'cc')), '-O2',
            '-std=gnu11', '-Wall', '-Wextra', '-Werror',
            '-I'+str(ROOT/'include'), '-I'+str(ROOT), str(src), '-o', str(exe)],
            check=True)
        lines = [str(len(cases))]
        for case in cases:
            raw = b''.join(case['insns']); ends=[]; n=0
            for insn in case['insns']:
                n += len(insn); ends.append(n)
            lines += [f'{len(raw)} {len(ends)} {int(case["fall"])} '
                      f'{int(case["relative"])}', ' '.join(map(str, ends)),
                      raw.hex(' ')]
        output = subprocess.check_output([str(exe)],
                                         input='\n'.join(lines).encode())
        constants = list(struct.unpack_from(f'<{len(CC_NAMES)}I', output))
        off=4*len(CC_NAMES)
        states=[list(struct.unpack_from('<5I',output,off+i*20))
                for i in range(128)]
        off+=128*20
        admitted=[]
        for case in cases:
            ok, = struct.unpack_from('<I',output,off);off+=4
            assert bool(ok)==case['valid'], case
            if not ok:
                continue
            delta, = struct.unpack_from('<I',output,off);off+=4
            raw=b''.join(case['insns'])
            case['length']=len(raw)
            expected_delta=len(raw)
            last=case['insns'][-1]
            if case['conditional'] or not case['fall']:
                size=4 if last[0] in [0x0f,0xe9] else 1
                expected_delta+=int.from_bytes(last[-size:], 'little',
                                               signed=True)
            assert delta==expected_delta&0xffffffff
            case['delta']=delta;case['modules']=[];case['tests']=[]
            for verify in range(2):
                size, = struct.unpack_from('<I',output,off);off+=4
                binary=tmp/f'{len(admitted)}-{verify}.wasm'
                binary.write_bytes(module(output[off:off+size]));off+=size
                case['modules'].append(str(binary))
            case['hex']=b''.join(case['insns']).hex()
            admitted.append(case)
        assert off==len(output)
        # IA32 runs original arithmetic bytes/conditions. Only Jcc displacement
        # is retargeted to a local capture stub; flags/GPRs are not changed.
        asm=['.section .text', '.global _start', '_start:']
        regs=['eax','ecx','edx','ebx','esp','ebp','esi','edi']; tests=[]
        for ci,case in enumerate(admitted):
            for seed in range(64 if case['exhaustive'] else 12):
                state=[rng.getrandbits(32) for _ in range(8)]
                if seed<4:
                    state=[0, 0xffffffff, 0x80008080, 0x7fff7f7f][seed:seed+1]*8
                lazy=states[seed if case['exhaustive'] else
                            (seed*11+ci)%len(states)]
                flags=lazy[4]&0x8d5
                test=dict(regs=state,flags=flags,lazy=lazy[:4],
                          pc=0xfffffff0 if seed==0 else rng.getrandbits(32))
                case['tests'].append(test)
                if not case['native']:
                    test['want']=state+[flags,0]
                    continue
                index=len(tests);tests.append(test)
                asm += [f'movl $stack_top, %esp', f'pushl ${flags|2}', 'popfl']
                asm += [f'movl ${value}, %{reg}'
                        for reg,value in zip(regs,state)]
                insns=case['insns']
                if case['conditional']:
                    insns=insns[:-1]
                if insns:
                    asm += ['.byte '+','.join(map(str,b''.join(insns)))]
                if case['conditional']:
                    branch=case['insns'][-1]
                    if branch[0]==0x0f:
                        asm += [f'.byte 0x0f,{branch[1]}',
                                f'.long taken_{index} - . - 4']
                    else:
                        asm += [f'.byte {branch[0]}',
                                f'.byte taken_{index} - . - 1']
                    asm += [f'movl $0,result+{index*40+36}',
                            f'jmp join_{index}', f'taken_{index}:',
                            f'movl $1,result+{index*40+36}', f'join_{index}:']
                asm += [f'movl %{reg}, result+{index*40+i*4}'
                        for i,reg in enumerate(regs)]
                asm += ['movl $stack_top, %esp', 'pushfl',
                        f'popl result+{index*40+32}']
        size=len(tests)*40
        asm += ['mov $4,%eax','mov $1,%ebx','mov $result,%ecx',
                f'mov ${size},%edx','int $0x80',
                'mov $1,%eax','xor %ebx,%ebx','int $0x80',
                '.section .bss','.align 16',f'.lcomm result,{size}',
                '.space 4096','stack_top:']
        (tmp/'oracle.s').write_text('\n'.join(asm)+'\n')
        subprocess.run(['as','--32',str(tmp/'oracle.s'),
                        '-o',str(tmp/'oracle.o')],check=True)
        subprocess.run(['ld','-melf_i386',str(tmp/'oracle.o'),
                        '-o',str(tmp/'oracle')],check=True)
        results=subprocess.check_output([str(tmp/'oracle')])
        assert len(results)==size
        for i,test in enumerate(tests):
            test['want']=list(struct.unpack_from('<10I',results,i*40))
        manifest=tmp/'cases.json'
        # Bytes are only needed by the native oracle, not the JSON consumer.
        for case in admitted:
            del case['insns']
        flags_wasm=tmp/'flags.wasm'
        manifest.write_text(json.dumps(dict(cc=constants,cases=admitted,
            states=states,flagsWasm=str(flags_wasm))))
        if args.wasm:
            flags_src=tmp/'flags.c';flags_src.write_text(FLAGS_C)
            clang=os.environ.get('WASM_CLANG',str(ROOT.parent /
                  'emsdk/upstream/bin/clang'))
            subprocess.run([clang,'--target=wasm32','-O2','-nostdlib',
                '-Wl,--no-entry','-Wl,--export=helper_cc_compute_all',
                '-I'+str(ROOT),str(flags_src),'-o',str(flags_wasm)],check=True)
            (tmp/'run.cjs').write_text(JS)
            subprocess.run([*shlex.split(os.environ.get('NODE','node')),
                str(tmp/'run.cjs'),str(manifest)],check=True)
        print(f'{len(admitted)} admitted / {len(cases)} bounded cases; '
              f'{len(tests)} native IA32 oracle executions; '
              + ('Wasm executed' if args.wasm else 'Wasm NOT executed'))


if __name__ == '__main__':
    main()
