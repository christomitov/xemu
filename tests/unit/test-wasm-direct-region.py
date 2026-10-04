#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Actual region collector/builder and retained-bank routing, mocked TB bodies.

Reference fragments model a small register/flags CFG; the real C CC evaluator
and differential verifier run as direct Wasm imports. Not an emulator gate.
"""
import importlib.util
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile


def load(name, file):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(file))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


R = load('region', 'test-wasm-region.py')
D = load('direct', 'test-wasm-direct.py')
ROOT = R.ROOT
PREFIX = R.PREFIX.replace('if (nimports || s0 || s1 || ic_count)',
                         'if (s0 || s1 || ic_count)').replace(
    'helper: { u: () => 1 },',
    '''helper: Object.fromEntries([['u', () => 1], ...Array.from(
        {length:nimports}, (_,i) => [String(i),
            getWasmTableEntry(HEAPU32[(imports>>2)+i])])]),''')

TEST = r'''
''' + D.FLAGS_C + r'''
struct CPUArchState {
    uint32_t regs[8], eip, cc_dst, cc_src, cc_src2, cc_op;
};
typedef struct CPUArchState CPUX86State;
typedef struct FixtureCPU {
    struct { int32_t icount_decr; bool can_do_io; } neg;
    CPUX86State env;
} FixtureCPU;
static FixtureCPU cpu;
#define env_cpu(e) ((FixtureCPU *)((char *)(e)-offsetof(FixtureCPU,env)))
''' + D.frontend_function('x86_wasm_direct_verify') + r'''
static unsigned refs, cc_calls, kick, mode, limit=3;
static bool profiling;
static CPUX86State initial;
static uint32_t fixture_cc(uint32_t dst,uint32_t src,uint32_t src2,int op) {
    assert(!strcmp((const char *)(uintptr_t)member_phase,
        mode==1 && profiling ? "direct:reg-cc" : "fixture:cc"));
    if(mode==1) {
        /* Pure production helper; test-only observer/kick, no reentry. */
        assert(!memcmp(cpu.env.regs,initial.regs,sizeof(initial.regs)));
    }
    if(++cc_calls==kick)cpu.neg.icount_decr=-1;
    return helper_cc_compute_all(dst,src,src2,op);
}
static uint32_t reference(WasmContext *ctx,unsigned i) {
    CPUX86State *e=ctx->env;refs++;cpu.neg.can_do_io=true;
    assert(i<3);
    if(i==0) {
        e->regs[0]++;e->cc_dst=e->regs[0]-limit;
        e->cc_src=limit;e->cc_op=CC_OP_SUBL;
        return e->regs[0]<limit?0:1;
    }
    if(i==1) {
        uint32_t cf=helper_cc_compute_all(e->cc_dst,e->cc_src,
                                        e->cc_src2,e->cc_op)&1;
        e->cc_dst=++e->regs[3];e->cc_src=cf;e->cc_op=CC_OP_INCL;
    }else e->regs[2]=0x12345678;
    return 0;
}
static void iconst(ByteBuf *b,uint32_t v) {bb_u8(b,0x41);bb_sleb(b,v);}
static void ctx_const(ByteBuf *b,unsigned off,uint32_t v) {
    bb_u8(b,0x20);bb_u8(b,0);iconst(b,v);
    bb_u8(b,0x36);bb_u8(b,2);bb_uleb(b,off);
}
static void env_pc(ByteBuf *b,uint32_t pc) {
    iconst(b,(uintptr_t)&cpu.env.eip);iconst(b,pc);
    bb_u8(b,0x36);bb_u8(b,2);bb_u8(b,0);
}
static void reloc(WasmReloc *r,unsigned *nr,ByteBuf *b,unsigned kind,
                  unsigned arg,unsigned depth) {
    r[(*nr)++]=(WasmReloc){.off=b->len,.kind=kind,.arg=arg,.depth=depth};
}
static void edge(ByteBuf *b,unsigned i,unsigned slot,unsigned depth,
                 WasmReloc *rl,unsigned *nr,uint32_t pc) {
    TranslationBlock *tb=&tbs[i];
    reloc(rl,nr,b,WASM_RELOC_DIRECT_VERIFY,slot,0);
    iconst(b,(uintptr_t)&tb->jmp_target_addr[slot]);
    bb_u8(b,0x28);bb_u8(b,2);bb_u8(b,0);
    bb_u8(b,0x22);bb_u8(b,REGION_L32_0);
    iconst(b,(uintptr_t)tb->tc.ptr+tb->jmp_reset_offset[slot]);
    bb_u8(b,0x47);bb_u8(b,4);bb_u8(b,0x40);
    reloc(rl,nr,b,WASM_RELOC_GOTO,slot,depth+1);
    bb_u8(b,0x20);bb_u8(b,0);bb_u8(b,0x20);bb_u8(b,REGION_L32_0);
    bb_u8(b,0x36);bb_u8(b,2);bb_u8(b,WASM_CTX_TB_PTR_OFF);
    ctx_const(b,WASM_CTX_DO_INIT_OFF,1);
    iconst(b,0);bb_u8(b,0x0f);bb_u8(b,0x0b);
    env_pc(b,pc);
    reloc(rl,nr,b,WASM_RELOC_DIRECT_VERIFY,255,0);
    ctx_const(b,WASM_CTX_TB_PTR_OFF,0);
    iconst(b,(uintptr_t)tb+slot);bb_u8(b,0x0f);
}
static void fixture(unsigned i,bool mixed,bool suspend) {
    static const uint8_t insns[3][12]={
        {0x05,1,0,0,0,0x3d,3,0,0,0,0x72,0x14},
        {0x43,0xe9,0xda,0xff,0xff,0xff},
        {0xba,0x78,0x56,0x34,0x12,0xe9,0x6a,0,0,0}
    };
    static const unsigned pc[]={0x1000,0x1020,0x100c};
    static const unsigned size[]={12,6,10};
    XwdTranslation t={.capture={.version=XWD_VERSION,.pc=pc[i],
        .size=size[i],.count=i==0?3:2,.valid=true,.fallthrough=false,
        .jump_valid=i==0?3:1,.end={i==1?1:5,i==1?6:10,12}},
        .layout={.verify=x86_wasm_direct_verify,.cc_all=fixture_cc,
        .phase_ptr=(uintptr_t)&member_phase,
        .phase_cc=(uintptr_t)"fixture:cc",
        .phase_generated=(uintptr_t)"fixture:generated",
        .regs=offsetof(CPUX86State,regs),.eip=offsetof(CPUX86State,eip),
        .cc_dst=offsetof(CPUX86State,cc_dst),
        .cc_src=offsetof(CPUX86State,cc_src),
        .cc_src2=offsetof(CPUX86State,cc_src2),.cc_op=offsetof(CPUX86State,cc_op),
        .icount_decr=offsetof(FixtureCPU,neg.icount_decr)-offsetof(FixtureCPU,env),
        .can_do_io=offsetof(FixtureCPU,neg.can_do_io)-offsetof(FixtureCPU,env),
        .cc_add={CC_OP_ADDB,CC_OP_ADDW,CC_OP_ADDL},
        .cc_sub={CC_OP_SUBB,CC_OP_SUBW,CC_OP_SUBL},
        .cc_logic={CC_OP_LOGICB,CC_OP_LOGICW,CC_OP_LOGICL},
        .cc_adc={CC_OP_ADCB,CC_OP_ADCW,CC_OP_ADCL},
        .cc_sbb={CC_OP_SBBB,CC_OP_SBBW,CC_OP_SBBL},
        .cc_inc={CC_OP_INCB,CC_OP_INCW,CC_OP_INCL},
        .cc_dec={CC_OP_DECB,CC_OP_DECW,CC_OP_DECL},.cc_eflags=CC_OP_EFLAGS}};
    t.capture.jump_pc[0]=i==0?0x1020:i==1?0x1000:0x1080;
    t.capture.jump_pc[1]=0x100c;
    memcpy(t.capture.bytes,insns[i],size[i]);
    XwdCode blueprint;assert(xwd_blueprint_write(&t,&blueprint));
    if(mixed && i==1)blueprint.bytes[0]=0;
    WasmTBHeader *h=&headers[i];TranslationBlock *tb=&tbs[i];
    tb->tc.ptr=h;tb->icount=t.capture.count;
    tb->jmp_reset_offset[0]=16;
    tb->jmp_reset_offset[1]=i==0?20:TB_JMP_OFFSET_INVALID;
    tb->jmp_target_addr[0]=i==0?(uintptr_t)&headers[1]:
        i==1?(uintptr_t)&headers[0]:(uintptr_t)h+16;
    tb->jmp_target_addr[1]=(uintptr_t)&headers[2];
    WasmReloc *rl=calloc(16,sizeof(*rl));unsigned nr=0;
    ByteBuf mod={0},sec={0},body={0};
    bb_bytes(&mod,"\0asm\x01\0\0\0",8);bb_uleb(&sec,4);
    bb_bytes(&sec,"\x60\x01\x7f\x01\x7f",5);
    bb_bytes(&sec,"\x60\x00\x01\x7f",4);
    bb_bytes(&sec,"\x60\x02\x7f\x7f\x01\x7f",6);
    bb_bytes(&sec,"\x60\x02\x7f\x7f\x01\x7f",6);
    bb_section(&mod,1,&sec);
    bb_name(&sec,XWD_BLUEPRINT_NAME);bb_bytes(&sec,blueprint.bytes,blueprint.size);
    bb_section(&mod,0,&sec);
    ctx_const(&body,WASM_CTX_DO_INIT_OFF,0);
    bb_u8(&body,3);bb_u8(&body,0x40); /* own TB loop */
    iconst(&body,(uintptr_t)&cpu.neg.icount_decr);
    bb_u8(&body,0x28);bb_u8(&body,2);bb_u8(&body,0);
    iconst(&body,0);bb_u8(&body,0x48);
    bb_u8(&body,4);bb_u8(&body,0x40);
    reloc(rl,&nr,&body,WASM_RELOC_DIRECT_VERIFY,255,0);
    ctx_const(&body,WASM_CTX_TB_PTR_OFF,0);
    iconst(&body,(uintptr_t)tb+3);bb_u8(&body,0x0f);bb_u8(&body,0x0b);
    reloc(rl,&nr,&body,WASM_RELOC_DIRECT_BEGIN,0,0);
    if(suspend && i==1)reloc(rl,&nr,&body,WASM_RELOC_DIRECT_SUSPEND,0,0);
    bb_u8(&body,0x20);bb_u8(&body,0);iconst(&body,i);bb_u8(&body,0x10);
    reloc(rl,&nr,&body,WASM_RELOC_CALL,0,0);
    bb_bytes(&body,"\x84\x80\x80\x80\x00",5);
    bb_u8(&body,0x21);bb_u8(&body,2);
    if(i==0) {
        bb_u8(&body,0x20);bb_u8(&body,2);bb_u8(&body,0x45);
        bb_u8(&body,4);bb_u8(&body,0x40);
        edge(&body,i,0,1,rl,&nr,t.capture.jump_pc[0]);
        bb_u8(&body,5);edge(&body,i,1,1,rl,&nr,t.capture.jump_pc[1]);
        bb_u8(&body,0x0b);
    }else edge(&body,i,0,0,rl,&nr,t.capture.jump_pc[0]);
    bb_u8(&body,0x0b);bb_u8(&body,0); /* end loop; unreachable */
    /* Framed opaque fragments: scanner sees a complete bounded section. */
    bb_name(&sec,"fixture-bodies");unsigned offa=sec.len;
    bb_bytes(&sec,body.p,body.len);unsigned offb=sec.len;
    bb_bytes(&sec,body.p,body.len);
    unsigned prefix=mod.len+1;for(unsigned v=sec.len;;v>>=7) {
        prefix++;if(v<128)break;
    }
    h->body_off=prefix+offa;h->body_b_off=prefix+offb;
    h->body_len=h->body_b_len=body.len;bb_section(&mod,0,&sec);
    h->wasm_ptr=mod.p;h->wasm_size=mod.len;h->counter=1000;
    h->reloc_ptr=h->reloc_b_ptr=rl;h->reloc_count=h->reloc_b_count=nr;
    uint32_t *imports=malloc(4);*imports=(uintptr_t)reference;
    h->import_ptr=imports;h->import_size=4;
    free(sec.p);free(body.p);
}
static WasmContext ctx;
static void reset(void) {
    cpu=(FixtureCPU){.neg.can_do_io=true,.env={.regs={0,2,0,7},
                   .eip=0x77777777,.cc_op=CC_OP_EFLAGS,.cc_src=1}};
    initial=cpu.env;refs=cc_calls=kick=0;
    ctx=(WasmContext){.env=&cpu.env,.tb_ptr=&headers[0],.do_init=1};
}
static uint32_t run(unsigned f) {
    return ((uint32_t(*)(WasmContext *))(uintptr_t)f)(&ctx);
}
int main(int argc,char **argv) {
    assert(argc==5);mode=atoi(argv[1]);
    profiling=argv[4][0]=='1';
    setenv("XEMU_WASM_DIRECT_PROFILE",argv[4],1);
    bool mixed=argv[2][0]=='1',suspend=argv[2][0]=='2';
    setenv("XEMU_WASM_DIRECT_X86",argv[1],1);
    setenv("XEMU_WASM_DIRECT_FLAGS","1",1);
    setenv("XEMU_WASM_DIRECT_REGIONS","1",1);
    setenv("XEMU_WASM_TLB_HINT",argv[3],1);
    setenv("XEMU_WASM_TB_STATS","0",1);
    for(unsigned i=0;i<3;i++)fixture(i,mixed,suspend);
    assert(!headers[0].instance && !headers[1].instance);
    int f=compile_region(&headers[0]);assert(f>0);
    assert(headers[0].instance==headers[1].instance &&
           headers[0].instance==headers[2].instance);
    assert(xemu_wasm_stats.n_region_members==3);
    assert(xemu_wasm_stats.n_direct_region_build==!(mixed||suspend));
    reset();assert(run(f)==(uintptr_t)&tbs[2] && !ctx.tb_ptr);
    assert(cpu.env.regs[0]==3 && cpu.env.regs[3]==9 &&
           cpu.env.regs[2]==0x12345678 && cpu.env.eip==0x1080);
    if(mixed||suspend) {
        assert(refs==6 && !xemu_wasm_stats.n_direct_region_checked);
        puts("PASS: whole selected mixed/unsafe group kept original TCG");
        return 0;
    }
    assert(!strcmp((const char *)(uintptr_t)member_phase,
        mode==1 && profiling ? "direct:reg" : "fixture:generated"));
    assert(refs==(mode==2?6:0));
    assert(xemu_wasm_stats.n_direct_region_checked==(mode==2?6:0));
    reset();cpu.env.regs[0]=2;initial=cpu.env;
    ctx.tb_ptr=&headers[1];assert(run(f)==(uintptr_t)&tbs[2]);
    assert(cpu.env.regs[0]==3 && cpu.env.regs[3]==8);
    reset();cpu.neg.icount_decr=-1;
    uint64_t before=xemu_wasm_stats.n_direct_region_checked;
    assert(run(f)==(uintptr_t)&tbs[0]+3 && !ctx.tb_ptr && !refs);
    assert(cpu.env.regs[0]==0 && cpu.env.regs[3]==7);
    assert(xemu_wasm_stats.n_direct_region_checked==before);
    reset();kick=1;before=xemu_wasm_stats.n_direct_region_checked;
    assert(run(f)==(uintptr_t)&tbs[1]+3 && !ctx.tb_ptr);
    assert(cpu.env.regs[0]==1 && cpu.env.regs[3]==7);
    assert(xemu_wasm_stats.n_direct_region_checked-before==(mode==2?1:0));
    reset();tbs[0].jmp_target_addr[0]=(uintptr_t)&headers[0]+16;
    assert(run(f)==(uintptr_t)&tbs[0] && !ctx.tb_ptr);
    assert(cpu.env.regs[0]==1 && cpu.env.eip==0x1020);
    for(unsigned k=0;k<4;k++) {
        reset();headers[7].instance=k==3?NULL:headers[0].instance;
        headers[7].instance_member=k==2?UINT32_MAX:1;
        tbs[0].jmp_target_addr[0]=(uintptr_t)(k==0?&headers[2]:&headers[7]);
        assert(run(f)==0 && ctx.tb_ptr==(void *)tbs[0].jmp_target_addr[0]);
        assert(cpu.env.regs[0]==1 && cpu.env.regs[3]==7);
    }
    assert(!xemu_wasm_stats.n_direct_mismatch);
    puts("PASS: actual direct region builder; retained/verified CFG, alias entry, "
         "per-member IRQ/skips, unlinked slot, changed/stale/foreign targets");
}
'''


def main():
    defines = '\n'.join(re.findall(r'^#define REGION_[^\n]*', R.SOURCE, re.M))
    names = ['wasm32_tlb_hint_enabled', 'bb_need', 'bb_u8', 'bb_bytes',
             'bb_uleb', 'bb_sleb', 'bb_name', 'bb_section', 'rd_uleb',
             'tb_helper_types', 'tb_link_target', 'region_leb', 'region_cfg',
             'region_member_ok', 'bb_count', 'region_guard', 'region_shared_guard',
             'add_instance', 'get_instance', 'remove_instances']
    stats = (ROOT/'include/qemu/xemu-wasm-stats.h').read_text()
    policy = '\n'.join(re.search(r'static inline bool ' + name +
                        r'\(void\).*?\n}', stats, re.S).group() for name in
                        ['xemu_wasm_tb_stats_enabled',
                         'xemu_wasm_direct_profile_enabled'])
    code = (PREFIX + policy + '\n' + defines + '\n' +
            '\n'.join(R.function(n) for n in names) +
            '\n#include "tcg/wasm32-direct-region.c.inc"\n' +
            R.function('compile_region') + TEST)
    with tempfile.TemporaryDirectory(prefix='direct-region-') as td:
        src, out = Path(td)/'test.c', Path(td)/'test.js'
        src.write_text(code)
        subprocess.run([*shlex.split(os.environ.get('CC', 'emcc')), '-O1',
                        '-DCONFIG_TCG_WASM_JIT=1', '-I'+str(ROOT),
                        '-I'+str(ROOT/'include'), '-pthread', '-sENVIRONMENT=node',
                        '-sINITIAL_MEMORY=32MB', '-sALLOW_TABLE_GROWTH=1',
                        '-sEXPORTED_RUNTIME_METHODS=addFunction,removeFunction',
                        '-sEXIT_RUNTIME=1', str(src), '-o', str(out)], check=True)
        env = {k: v for k, v in os.environ.items() if not k.startswith('XEMU_')}
        for mode in [1, 2]:
            for scenario in [0, 1, 2]:
                for hints in [0, 1]:
                    for profile in [0, 1]:
                        subprocess.run([*shlex.split(os.environ.get('NODE', 'node')),
                                        str(out), str(mode), str(scenario),
                                        str(hints), str(profile)],
                                       env=env, check=True, timeout=30)


if __name__ == '__main__':
    main()
