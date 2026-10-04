#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Actual-source census classifier/counters/emitter fixtures, not an emulator.

CC defaults to cc. NODE can name a guarded Node wrapper; --wasm runs the tiny
emitted Wasm module. Full emulator region/rewind/entry coverage is separate.
"""
import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
CLASSIFY = (ROOT / 'target/i386/tcg/wasm-census.c.inc').read_text()
BACKEND = (ROOT / 'tcg/wasm32/tcg-target.c.inc').read_text()
FRONTEND = (ROOT / 'target/i386/tcg/translate.c').read_text()
IR = (ROOT / 'tcg/tcg-op.c').read_text()


def function(source, name):
    match = re.search(r'^(?:static )?[^\n]*\b' + name + r'\(', source, re.M)
    assert match, name
    brace = source.index('{', match.start())
    depth = 0
    for token in re.finditer(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|'
                             r"'(?:\\.|[^'\\])*'|[{}]", source[brace:], re.S):
        if token.group() == '{':
            depth += 1
        elif token.group() == '}':
            depth -= 1
            if not depth:
                return source[match.start():brace + token.end()] + '\n'
    raise AssertionError(name)


PREFIX = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define CONFIG_TCG_WASM_JIT 1
#define EMSCRIPTEN 1
#include "qemu/xemu-wasm-stats.h"
XemuWasmStats xemu_wasm_stats;
typedef void (*X86GenFunc)(void);
enum { X86_OP_SKIP, X86_OP_SEG, X86_OP_CR, X86_OP_DR,
       X86_OP_INT, X86_OP_IMM, X86_OP_SSE, X86_OP_MMX };
enum { MO_8, MO_16, MO_32 };
enum { PREFIX_LOCK=1, PREFIX_REPZ=2, PREFIX_REPNZ=4, PREFIX_VEX=8 };
enum { X86_CHECK_cpl0=1, X86_CHECK_iopl=2, X86_CHECK_smm=4 };
typedef struct {
    struct { X86GenFunc gen; unsigned check; } e;
    struct { unsigned unit; bool has_ea; } op[3];
    uint32_t immediate;
} X86DecodedInsn;
typedef struct { unsigned cflags; } TranslationBlock;
typedef struct { TranslationBlock *tb; uint32_t pc_first; } DisasContextBase;
typedef struct { int unused; } CPUState;
typedef struct {
    DisasContextBase base;
    unsigned flags, cs_base, pc;
    bool cpu_has_bps;
    uint32_t *wasm_census_word;
    bool wasm_census_classified;
    unsigned aflag, dflag, prefix;
    int override;
} DisasContext;
static void x86_wasm_direct_start(DisasContext *s) { (void)s; }
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define PE(s) 1
#define CODE32(s) 1
#define SS32(s) 1
#define CODE64(s) 0
#define ADDSEG(s) 0
#define VM86(s) 0
#define GUEST(s) 0
#define SVME(s) 0
#define HF_TF_MASK 1
#define HF_RF_MASK 2
#define HF_INHIBIT_IRQ_MASK 4
#define CF_COUNT_MASK 255
#define CF_NOIRQ (1<<8)
#define CF_SINGLE_STEP (1<<9)
#define CF_BP_PAGE (1<<10)
#define CF_MEMI_ONLY (1<<11)
#define CF_PARALLEL (1<<12)
#define CF_USE_ICOUNT (1<<13)
#define tb_cflags(tb) ((tb)->cflags)
#define tcg_malloc malloc
#define INDEX_op_wasm_census 241
#define TCG_TYPE_I32 0
static unsigned markers;
static const uint32_t *patched;
static void tcg_gen_op1(int op, int type, uintptr_t arg) {
    assert(op == INDEX_op_wasm_census && type == TCG_TYPE_I32);
    patched=(const uint32_t *)arg; markers++;
}
'''

TEST = r'''
static uint32_t record(X86GenFunc gen, int unit, int mem,
                       unsigned prefix, int override, int dflag) {
    uint32_t word = XWC_ELIGIBLE | XWC_REGONLY;
    DisasContext s = { .wasm_census_word=&word, .aflag=MO_32,
                      .dflag=dflag, .override=override, .prefix=prefix };
    X86DecodedInsn d = { .e.gen=gen, .op={{.unit=unit,.has_ea=mem}} };
    wasm_census_record(&s, &d);
    assert(s.wasm_census_classified);
    return word;
}
#define R(g,u,m) record(gen_##g, u, m, 0, -1, MO_32)
int main(void) {
    const char *env = getenv("XEMU_WASM_INSN_CENSUS");
    assert(wasm_census_enabled() == (env && *env == '1'));
    TranslationBlock tb={0};
    DisasContext front={.base.tb=&tb};
    i386_tr_tb_start(&front.base,NULL);
    assert(markers == !!(env && *env == '1'));
    if (markers) {
        assert(patched==front.wasm_census_word);
        assert(*patched==(XWC_ELIGIBLE|XWC_REGONLY));
        *front.wasm_census_word|=3<<8;
        /* Final TB length patched, not captured early. */
        assert((*patched>>8)==3);
        free(front.wasm_census_word);
        for (unsigned bit=1;bit<=CF_USE_ICOUNT;bit<<=1) {
            tb.cflags=bit;
            i386_tr_tb_start(&front.base,NULL);
            assert(!(*patched&XWC_ELIGIBLE));
            free(front.wasm_census_word);
        }
        tb.cflags=0; front.cpu_has_bps=true;
        i386_tr_tb_start(&front.base,NULL);
        assert(!(*patched&XWC_ELIGIBLE)); free(front.wasm_census_word);
    } else { assert(!front.wasm_census_word); }
    assert(R(ADD,X86_OP_INT,0) == (XWC_ELIGIBLE|XWC_REGONLY));
    assert(R(MOV,X86_OP_INT,1) == XWC_ELIGIBLE);
    assert(R(LEA,X86_OP_INT,1) == (XWC_ELIGIBLE|XWC_REGONLY));
    assert(R(CALL,X86_OP_INT,0) == XWC_ELIGIBLE);
    assert(R(RET,X86_OP_INT,0) == XWC_ELIGIBLE);
    assert(R(DIV,X86_OP_INT,0) == XWC_REGONLY);
    assert((R(MOV,X86_OP_SEG,0) & XWC_MASK) == XWC_SYSTEM);
    assert((R(MOV,X86_OP_SSE,1) & XWC_MASK) == XWC_SSE);
    assert((R(MOV,X86_OP_MMX,1) & XWC_MASK) == XWC_MMX);
    assert((R(EMMS,X86_OP_SKIP,0) & XWC_MASK) == XWC_MMX);
    assert((R(WAIT,X86_OP_SKIP,0) & XWC_MASK) == XWC_X87);
    assert((R(x87,X86_OP_SKIP,0) & XWC_MASK) == XWC_X87);
    assert((R(LDMXCSR,X86_OP_SKIP,1) & XWC_MASK) == XWC_SSE);
    assert((R(FXSAVE,X86_OP_SKIP,1) & XWC_MASK) == 15);
    assert((R(MOVS,X86_OP_SKIP,0) & XWC_MASK) == XWC_STRING);
    assert((R(INS,X86_OP_SKIP,0) & XWC_MASK) == (XWC_STRING|XWC_SYSTEM));
    assert((R(RDTSC,X86_OP_INT,0) & XWC_MASK) == XWC_SYSTEM);
    assert((R(unknown,X86_OP_SKIP,0) & XWC_MASK) == XWC_OTHER);
    for (int bit=1; bit<=8; bit*=2) {
        assert(!(record(gen_ADD,X86_OP_INT,0,bit,-1,MO_32)&XWC_ELIGIBLE));
    }
    assert(!(record(gen_MOV,X86_OP_INT,0,0,4,MO_32)&XWC_ELIGIBLE));
    assert(!(record(gen_ADD,X86_OP_INT,0,0,-1,MO_16)&XWC_ELIGIBLE));
    uint32_t word=XWC_ELIGIBLE|XWC_REGONLY;
    DisasContext s={ .wasm_census_word=&word, .aflag=MO_32,
                    .dflag=MO_32, .override=-1 };
    X86DecodedInsn d={ .e.gen=gen_MOV, .op={{.unit=X86_OP_SSE},
                                        {.unit=X86_OP_MMX}} };
    wasm_census_record(&s,&d);
    assert((word&XWC_MASK)==(XWC_SSE|XWC_MMX));
    d.e.gen=gen_x87; d.op[0].unit=d.op[1].unit=X86_OP_SKIP;
    wasm_census_record(&s,&d);
    assert((word&XWC_MASK)==7 && !(word&XWC_ELIGIBLE));
    s.wasm_census_word=NULL; s.wasm_census_classified=false;
    wasm_census_record(&s,&d); assert(!s.wasm_census_classified);
    for (unsigned i=0;i<64;i++) {
        xemu_wasm_census_hit((512u<<8)|i);
        assert(*xemu_wasm_census_counter(i,0)==1);
        assert(*xemu_wasm_census_counter(i,1)==512);
    }
    xemu_wasm_census_hit((3u<<8)|XWC_ELIGIBLE|XWC_REGONLY);
    xemu_wasm_census_hit((5u<<8)|XWC_ELIGIBLE);
    assert(*xemu_wasm_census_counter(0x40,0)==2);
    assert(*xemu_wasm_census_counter(0x40,1)==8);
    assert(*xemu_wasm_census_counter(0x41,0)==1);
    assert(*xemu_wasm_census_counter(0x41,1)==3);
    /* Carry through 2^32, not uint32 wrapping. */
    *xemu_wasm_census_counter(0,1)=UINT32_MAX;
    xemu_wasm_census_hit((5u<<8)|XWC_ELIGIBLE);
    assert(*xemu_wasm_census_counter(0,1)==(uint64_t)UINT32_MAX+5);
    puts("classification/counters OK");
}
'''

EMITTER = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "qemu/xemu-wasm-census.h"
#define EMSCRIPTEN 1
#define tcg_debug_assert assert
#define INDEX_op_wasm_census 241
#define W_I64_LOAD 0x29
#define W_I64_STORE 0x37
#define W_I64_ADD 0x7c
typedef struct {
    unsigned char buf[65536]; unsigned n; uint32_t tci;
} TCGContext;
static void wasm8(TCGContext *s,int v) { s->buf[s->n++]=v; }
static void uleb(TCGContext *s,unsigned v) {
    do { unsigned b=v&127; v>>=7; wasm8(s,b|(v?128:0)); } while(v);
}
static void sleb(TCGContext *s,int64_t v) {
    for (;;) { int b=v&127; v>>=7;
        bool more=!((v==0 && !(b&64)) || (v==-1 && (b&64)));
        wasm8(s,b|(more?128:0)); if(!more)break;
    }
}
static void wasm_i32_const(TCGContext *s,int32_t v) {
    wasm8(s,0x41); sleb(s,v);
}
static void wasm_i64_const(TCGContext *s,int64_t v) {
    wasm8(s,0x42); sleb(s,v);
}
static void wasm_mem(TCGContext *s,int op,int off) {
    wasm8(s,op); uleb(s,3); uleb(s,off);
}
static void tcg_out32(TCGContext *s,uint32_t v) { s->tci=v; }
static uint64_t *xemu_wasm_census_counter(unsigned b,bool insns) {
    return (uint64_t *)(uintptr_t)(4096+b*16+insns*8);
}
'''
EMITTER_MAIN = r'''
int main(void) {
    TCGContext s={0};
    for(unsigned i=0;i<64;i++) {
        uint32_t word=((i+1)<<8)|i;
        wasm_census(&s,word);
        assert((s.tci&255)==INDEX_op_wasm_census && (s.tci>>8)==word);
    }
    wasm_census(&s,(3<<8)|XWC_ELIGIBLE|XWC_REGONLY);
    wasm_census(&s,(5<<8)|XWC_ELIGIBLE);
    assert(fwrite(s.buf,1,s.n,stdout)==s.n);
}
'''


def uleb(x):
    out = bytearray()
    while True:
        byte = x & 127
        x >>= 7
        out.append(byte | (128 if x else 0))
        if not x:
            return out


def section(n, payload):
    return bytes([n]) + uleb(len(payload)) + payload


def main():
    args = argparse.ArgumentParser()
    args.add_argument('--wasm', action='store_true')
    run_wasm = args.parse_args().wasm
    cc = shlex.split(os.environ.get('CC', 'cc'))
    gens = sorted(set(re.findall(r'\bgen_\w+', CLASSIFY)) | {'gen_unknown'})
    stubs = '\n'.join(f'static void {g}(void) {{}}' for g in gens)
    with tempfile.TemporaryDirectory(prefix='wasm-census-') as tmp:
        tmp = Path(tmp)
        def build(text, name):
            src, exe = tmp / (name+'.c'), tmp / name
            src.write_text(text)
            subprocess.run([*cc, '-std=gnu11', '-O2', '-Wall', '-Wextra',
                            '-Werror', '-Wno-unused-parameter',
                            '-I'+str(ROOT/'include'), str(src),
                            '-o', str(exe)], check=True)
            return exe
        exe = build(PREFIX+stubs+'\n'+CLASSIFY+
                    function(IR, 'tcg_gen_wasm_census')+
                    function(FRONTEND, 'i386_tr_tb_start')+TEST, 'classifier')
        for mode in [None, '', '0', '1', '2', 'invalid']:
            env = dict(os.environ)
            env.pop('XEMU_WASM_INSN_CENSUS', None)
            if mode is not None:
                env['XEMU_WASM_INSN_CENSUS'] = mode
            subprocess.run([str(exe)], env=env, check=True)
        emit = build(EMITTER+function(BACKEND,'wasm_count')+
                     function(BACKEND,'wasm_census')+EMITTER_MAIN, 'emitter')
        body = b'\0'+subprocess.check_output([str(emit)])+b'\x0b'
        wasm = b'\0asm\x01\0\0\0'
        wasm += section(1, b'\x01\x60\x00\x00')
        wasm += section(2, b'\x01\x03env\x06memory\x02\x00\x01')
        wasm += section(3, b'\x01\x00')
        wasm += section(7, b'\x01\x03run\x00\x00')
        wasm += section(10, b'\x01'+uleb(len(body))+body)
        binary = tmp/'fixture.wasm'
        binary.write_bytes(wasm)
        if run_wasm:
            js = tmp/'run.cjs'
            js.write_text('''const fs=require('fs'), assert=require('assert');
const memory=new WebAssembly.Memory({initial:1});
const code=new WebAssembly.Module(fs.readFileSync(process.argv[2]));
const m=new WebAssembly.Instance(code,{env:{memory}});
const v=new BigUint64Array(memory.buffer,4096,132);
for(let k=0;k<100;k++)m.exports.run();
for(let i=0;i<64;i++){
  assert.equal(v[i*2],BigInt(i===0?300:100));
  assert.equal(v[i*2+1],BigInt(i===0?900:(i+1)*100));
}
assert.equal(v[128],200n);assert.equal(v[129],800n);
assert.equal(v[130],100n);assert.equal(v[131],300n);
console.log('real Wasm emitted counter updates OK');
''')
            subprocess.run([*shlex.split(os.environ.get('NODE','node')),
                            str(js), str(binary)], check=True)
        suffix = ' including real Wasm' if run_wasm else ' (no Node run)'
        print('census fixtures passed'+suffix)


if __name__ == '__main__':
    main()
