#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Actual-source class metadata, sampler and emitted phase-store fixtures.

Decoded descriptors/TCG lists are mocked, not the real x86 decoder. --wasm
executes emitted stores, not an emulator or a real Asyncify continuation.
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('census',
    ROOT / 'tests/unit/test-wasm-census.py')
c = importlib.util.module_from_spec(spec)
spec.loader.exec_module(c)
f = c.function
UI = (ROOT / 'ui/xemu-wasm.c').read_text()
STATS = (ROOT / 'include/qemu/xemu-wasm-stats.h').read_text()
start = UI.index('const char *const xemu_wasm_insn_phase_names')
NAMES = UI[start:UI.index('};', start) + 2] + '\n'

META = r'''
typedef struct TCGOp { int opc; uintptr_t args[1]; struct TCGOp *next; } TCGOp;
typedef struct { TCGOp *ops; } TCGContext;
#define QTAILQ_FOREACH(op,head,unused) for (op=*(head); op; op=op->next)
static XemuWasmInsnPhase wasm_tb_insn_class = XWIP_UNKNOWN;
'''
TEST = r'''
int main(void) {
    const char *p=getenv("XEMU_WASM_INSN_PROFILE");
    const char *ce=getenv("XEMU_WASM_INSN_CENSUS");
    bool profile=p && *p=='1', census=ce && *ce=='1';
    assert(xemu_wasm_insn_profile_enabled()==profile);
    /* PROFILE=0 and TB_STATS=0: only the new explicit flag forces sampling. */
    assert(xemu_wasm_profile_enabled()==profile);
    assert(!xemu_wasm_tb_stats_enabled());
    TranslationBlock tb={0};
    DisasContext s={.base={.tb=&tb,.pc_first=0x12345000},
                   .aflag=MO_32,.dflag=MO_32,.override=-1};
    i386_tr_tb_start(&s.base,NULL);
    assert(markers==!!(profile||census));
    if (s.wasm_census_word) {
        assert(!!(*s.wasm_census_word&XWC_NOCOUNT)==!census);
        assert(*s.wasm_census_word&XWC_ELIGIBLE);
        /* Generic descriptors for load / mov / sub / cmp / backward jcc. */
        X86DecodedInsn d={.e.gen=gen_MOV,
                         .op={{.unit=X86_OP_INT,.has_ea=true}}};
        wasm_census_record(&s,&d);
        d.op[0].has_ea=false;
        wasm_census_record(&s,&d);
        d.e.gen=gen_SUB;
        wasm_census_record(&s,&d); wasm_census_record(&s,&d);
        d.e.gen=gen_Jcc; s.pc=s.base.pc_first+10; d.immediate=(uint32_t)-10;
        wasm_census_record(&s,&d);
        assert(!!(*s.wasm_census_word&XWC_SELFLOOP)==profile);
        assert(*s.wasm_census_word&XWC_ELIGIBLE);
        /* 32-bit address wrap and register/PC-independent detection. */
        for(unsigned i=0;i<4;i++) {
            uint32_t starts[]={0,0xfffffff6u,0x40000000u,0x80000000u};
            for(unsigned j=0;j<2;j++) {
                *s.wasm_census_word=XWC_ELIGIBLE;
                s.base.pc_first=starts[i]; s.pc=starts[i]+10;
                d.e.gen=j ? gen_JMP : gen_Jcc;
                wasm_census_record(&s,&d);
                assert(!!(*s.wasm_census_word&XWC_SELFLOOP)==profile);
                *s.wasm_census_word=XWC_ELIGIBLE;
                d.immediate=(uint32_t)-9;
                wasm_census_record(&s,&d);
                assert(!(*s.wasm_census_word&XWC_SELFLOOP));
                d.immediate=(uint32_t)-10;
            }
        }
        /* Not a direct branch, nonflat/16-bit: no self-loop claim. */
        *s.wasm_census_word=XWC_ELIGIBLE; d.e.gen=gen_CALL;
        wasm_census_record(&s,&d); assert(!(*s.wasm_census_word&XWC_SELFLOOP));
        d.e.gen=gen_Jcc; s.cs_base=1;
        wasm_census_record(&s,&d); assert(!(*s.wasm_census_word&XWC_SELFLOOP));
        s.cs_base=0; s.dflag=MO_16;
        wasm_census_record(&s,&d); assert(!(*s.wasm_census_word&XWC_SELFLOOP));
        free(s.wasm_census_word);
    }
    uint32_t word=0;
    TCGOp marker={.opc=INDEX_op_wasm_census,.args={(uintptr_t)&word}};
    TCGOp prior={.opc=0,.next=&marker};
    TCGContext ctx={.ops=&prior};
    for(unsigned k=0;k<4;k++) {
        word=(5<<8)|XWC_NOCOUNT|((k<2)?XWC_ELIGIBLE:0)|
             ((k&1)?XWC_SELFLOOP:0);
        wasm_insn_profile_start(&ctx);
        assert(wasm_tb_insn_class==(profile?k:XWIP_UNKNOWN));
        const char *scopes[]={NULL,"jit:ld","jit:st","jit:lookup",
                              "jit:cc","jit:call","jit:x87","jit:mmx"};
        for(unsigned j=0;j<ARRAY_SIZE(scopes);j++) {
            assert(wasm_insn_phase_name(scopes[j])==
                   (profile?xemu_wasm_insn_phase_names[k]:scopes[j]));
        }
        const char *spin="spin";
        assert(wasm_insn_phase_name(spin)==
               (profile?xemu_wasm_insn_phase_names[k|1]:spin));
        const char *outside[]={"mem_slow_ld","mem_slow_st","rdtsc",
                                "notdirty_inline","x87","dispatch"};
        for(unsigned j=0;j<ARRAY_SIZE(outside);j++) {
            assert(wasm_insn_phase_name(outside[j])==outside[j]);
        }
    }
    ctx.ops=NULL; wasm_insn_profile_start(&ctx);
    assert(wasm_tb_insn_class==XWIP_UNKNOWN); /* never retain previous TB */
    for(unsigned k=0;k<XWIP_COUNT;k++) {
        for(unsigned n=0;n<=k;n++) {
            xemu_wasm_insn_sample(xemu_wasm_insn_phase_names[k]);
        }
    }
    xemu_wasm_insn_sample(NULL);
    xemu_wasm_insn_sample("mem_slow_st");
    assert(xemu_wasm_stats.n_insn_sample_total==17);
    assert(xemu_wasm_stats.n_insn_sample_eligible==1);
    assert(xemu_wasm_stats.n_insn_sample_eligible_loop==2);
    assert(xemu_wasm_stats.n_insn_sample_other==3);
    assert(xemu_wasm_stats.n_insn_sample_other_loop==4);
    assert(xemu_wasm_stats.n_insn_sample_unknown==6);
    puts("metadata/policy/loop classification/phase restoration/sampler OK");
}
'''
EMIT_EXTRA = r'''
#define XPHASE_VCPU 0
#define xemu_wasm_phase ((const char *volatile *)(uintptr_t)128)
const char *const xemu_wasm_insn_phase_names[XWIP_COUNT] = {
    (const char *)16, (const char *)32, (const char *)48,
    (const char *)64, (const char *)80
};
static XemuWasmInsnPhase wasm_tb_insn_class = XWIP_UNKNOWN;
'''
EMIT_MAIN = r'''
int main(int argc,char **argv) {
    assert(argc==2);
    TCGContext a={0}, b={0}, none={.tci=0xdeadbeef};
    uint32_t word=(7<<8)|XWC_ELIGIBLE;
    wasm_census(&a,word); wasm_census(&b,word|XWC_SELFLOOP);
    assert(a.n==b.n && a.tci==b.tci && !memcmp(a.buf,b.buf,a.n));
    wasm_census(&none,word|XWC_NOCOUNT|XWC_SELFLOOP);
    assert(none.n==0 && none.tci==0xdeadbeef);
    const char *inputs[]={NULL,"spin","jit:ld","jit:lookup","jit:call",
                           "mem_slow_st",NULL};
    for(unsigned k=0;k<XWIP_COUNT;k++) {
        wasm_tb_insn_class=k;
        for(unsigned j=0;j<sizeof(inputs)/sizeof(inputs[0]);j++) {
            TCGContext s={0};
            wasm_phase(&s,inputs[j]);
            char path[1024];
            snprintf(path,sizeof(path),"%s/%u-%u.bin",argv[1],k,j);
            FILE *fp=fopen(path,"wb"); assert(fp);
            assert(fwrite(s.buf,1,s.n,fp)==s.n); fclose(fp);
            uint32_t value=xemu_wasm_profile_enabled() ?
                (uint32_t)(uintptr_t)wasm_insn_phase_name(inputs[j]):0x5a5a5a5a;
            printf("%u %u %u\n",k,j,value);
        }
    }
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--wasm', action='store_true')
    args = parser.parse_args()
    report_spec = importlib.util.spec_from_file_location('report',
        ROOT / 'scripts/wasm-insn-profile-report.py')
    report = importlib.util.module_from_spec(report_spec)
    report_spec.loader.exec_module(report)
    values = {'n_insn_sample_' + k: [v, 999, 0, 1000] for k, v in {
        'total': 1000, 'eligible': 200, 'eligible_loop': 300,
        'other': 250, 'other_loop': 50, 'unknown': 100}.items()}
    result = report.summarize({'report': {'summary_last60s': values}})
    assert result['vcpu_pct']['eligible_nonself'] == 20
    assert result['eligible_nonself_pct_of_classified_jit'] == 25
    key = 'eligible_nonself_pct_including_unknown_denominator'
    assert result[key] == 22.2222
    assert result['all_candidate_pct_of_classified_jit_NOT_gain_share'] == 62.5
    assert result['conditional_speedup_pct']['20'] == 4.1667
    for invalid in [{}, {'n_insn_sample_total': 0},
                    {'n_insn_sample_total': 1, 'n_insn_sample_eligible': 2},
                    {'n_insn_sample_total': 100, 'n_insn_sample_other': -1}]:
        try:
            report.summarize(invalid)
        except ValueError:
            pass
        else:
            raise AssertionError('invalid report accepted')
    gens = sorted(set(re.findall(r'\bgen_\w+', c.CLASSIFY)))
    stubs = '\n'.join(f'static void {g}(void) {{}}' for g in gens)
    code = (c.PREFIX + stubs + '\n' + c.CLASSIFY +
            f(c.IR, 'tcg_gen_wasm_census') + f(c.FRONTEND, 'i386_tr_tb_start') +
            META + NAMES + f(c.BACKEND, 'wasm_insn_profile_start') +
            f(c.BACKEND, 'wasm_insn_phase_name') +
            f(UI, 'xemu_wasm_insn_sample') + TEST)
    emitter = ('#include <string.h>\n#define W_I32_STORE 0x36\n' +
               c.EMITTER.replace('uleb(s,3);', 'uleb(s,op==W_I32_STORE?2:3);') +
               f(STATS, 'xemu_wasm_insn_profile_enabled') +
               f(STATS, 'xemu_wasm_tb_stats_enabled') +
               f(STATS, 'xemu_wasm_profile_enabled') + EMIT_EXTRA +
               f(c.BACKEND, 'wasm_count') + f(c.BACKEND, 'wasm_census') +
               f(c.BACKEND, 'wasm_insn_phase_name') +
               f(c.BACKEND, 'wasm_phase') + EMIT_MAIN)
    with tempfile.TemporaryDirectory(prefix='wasm-insn-profile-') as td:
        tmp = Path(td)
        def build(text, name):
            src, exe = tmp / (name + '.c'), tmp / name
            src.write_text(text)
            subprocess.run([*shlex.split(os.environ.get('CC', 'cc')),
                '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
                '-Wno-unused-parameter', '-I' + str(ROOT/'include'),
                str(src), '-o', str(exe)], check=True)
            return exe
        exe, emit = build(code, 'classifier'), build(emitter, 'emitter')
        cases = []
        for profile in [None, '', '0', '1', 'invalid']:
            for census in ['0', '1']:
                env = {k:v for k,v in os.environ.items()
                       if not k.startswith('XEMU_')}
                env.update(XEMU_WASM_TB_STATS='0', XEMU_WASM_PROFILE='0',
                           XEMU_WASM_INSN_CENSUS=census)
                if profile is not None:
                    env['XEMU_WASM_INSN_PROFILE'] = profile
                subprocess.run([str(exe)], env=env, check=True)
                folder = tmp / str(len(cases)); folder.mkdir()
                expected = subprocess.check_output([str(emit), str(folder)],
                                                   env=env, text=True)
                for line in expected.splitlines():
                    k, j, value = map(int, line.split())
                    body = (folder / f'{k}-{j}.bin').read_bytes()
                    assert bool(body) == (profile == '1')
                    module = b'\0asm\x01\0\0\0'
                    module += c.section(1, b'\x01\x60\x00\x00')
                    module += c.section(2, b'\x01\x03env\x06memory\x02\x00\x01')
                    module += c.section(3, b'\x01\x00')
                    module += c.section(7, b'\x01\x03run\x00\x00')
                    body = b'\0' + body + b'\x0b'
                    module += c.section(10, b'\x01' + c.uleb(len(body)) + body)
                    binary = folder / f'{k}-{j}.wasm'
                    binary.write_bytes(module)
                    cases.append([str(binary), value])
        if args.wasm:
            manifest = tmp / 'cases.json'
            manifest.write_text(json.dumps(cases))
            js = tmp / 'run.cjs'; js.write_text('''
const fs=require('fs'), assert=require('assert');
const cases=JSON.parse(fs.readFileSync(process.argv[2]));
const memory=new WebAssembly.Memory({initial:1});
for(const [path,want] of cases){
 const bytes=fs.readFileSync(path); assert(WebAssembly.validate(bytes));
 const m=new WebAssembly.Instance(new WebAssembly.Module(bytes),{env:{memory}});
 const b=new Uint8Array(memory.buffer); b.fill(0x5a);
 for(let n=0;n<100;n++)m.exports.run();
 const v=new DataView(memory.buffer); assert.equal(v.getUint32(128,true),want);
 b.fill(0x5a,128,132); assert(b.every(x=>x===0x5a));
}
console.log(cases.length+' phase modules executed; only phase word changed');
''')
            subprocess.run([*shlex.split(os.environ.get('NODE', 'node')),
                            str(js), str(manifest)], check=True)
        print('instruction-profile actual-source fixtures passed' +
              (' with emitted Wasm' if args.wasm else ' (no Node run)'))


if __name__ == '__main__':
    main()
