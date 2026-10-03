#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run the compact lookup emitter against the original frontend's TCG stream.

The frontend emits a tiny fixture IR; its interpreter is the oracle. The
candidate is real WebAssembly made by the production emitter. Layouts are
fixtures (including negative env offsets); the full Wasm build separately
checks the real layout initializers. No emulator/game or guest I/O is run.
"""

import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
BACK = (ROOT / "tcg/wasm32/tcg-target.c.inc").read_text()
FRONT = (ROOT / "target/i386/tcg/translate.c").read_text()
HEADER = (ROOT / "tcg/wasm32.h").read_text()


def function(source, name):
    m = re.search(r"(?m)^static [\w *]+\b" + name + r"\([^;]*?\)\n\{",
                  source)
    assert m, name
    start = source.index("{", m.start())
    end = source.index("\n}", start) + 2
    return source[m.start():end] + "\n"


C = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define TARGET_PAGE_BITS 12
#define TARGET_LONG_BITS 32
#define TCG_AREG0 14
#define R_CS 1
#define IOPL_MASK (3u << 12)
#define TF_MASK (1u << 8)
#define RF_MASK (1u << 16)
#define VM_MASK (1u << 17)
#define AC_MASK (1u << 18)
#define QEMU_BUILD_BUG_ON(x) _Static_assert(!(x), #x)
#define LREG(r) (8 + (r))
enum { L32_0 = 1, L32_1, L32_2, L32_3, L64_0 };
typedef unsigned TCGReg;
#if PC64
typedef uint64_t vaddr;
#else
typedef uint32_t vaddr;
#endif
typedef struct { unsigned n; unsigned char b[4096]; } TCGContext;
static void wasm8(TCGContext *s, unsigned b) {
    assert(s->n < sizeof(s->b)); s->b[s->n++] = b;
}
static void wasm_uleb(TCGContext *s, uint32_t v) {
    do { unsigned b = v & 127; v >>= 7;
         wasm8(s, b | (v ? 128 : 0)); } while (v);
}
static void wasm_var(TCGContext *s, unsigned op, unsigned v) {
    wasm8(s, op); wasm_uleb(s, v);
}
static void wasm_i32_const(TCGContext *s, int32_t v) {
    wasm8(s, 0x41);
    for (;;) {
        unsigned b = v & 127; v >>= 7;
        bool end = (v == 0 && !(b & 64)) || (v == -1 && (b & 64));
        wasm8(s, b | (end ? 0 : 128)); if (end) { break; }
    }
}
static void wasm_mem(TCGContext *s, unsigned op, unsigned offset) {
    wasm8(s, op); wasm_uleb(s, 2); wasm_uleb(s, offset);
}
static void wasm_get_r32(TCGContext *s, TCGReg r) {
    wasm_var(s, 0x20, LREG(r)); wasm8(s, 0xa7);
}
static void wasm_jit_phase(TCGContext *s, unsigned mask, const char *name) {
    assert(mask == 8); /* Phase stores tested in test-wasm-phase.py. */
}

typedef struct { uint32_t regs[8], eip, eflags, hflags;
                 struct { uint32_t base; } segs[6]; } CPUX86State;
typedef struct { uint32_t tb_jmp_cache;
                 struct { uint32_t tqh_first; } breakpoints; } CPUState;
typedef struct { CPUState parent_obj; uint32_t pad[PAD];
                 CPUX86State env; } X86CPU;
typedef struct { uint32_t pad[PAD], gen;
                 struct { uint32_t tb; vaddr pc; uint32_t gen; }
                 array[32768]; } CPUJumpCache;
typedef struct { uint32_t pad[PAD], cflags, flags, cs_base;
                 struct { uint32_t ptr; } tc; } TranslationBlock;
typedef struct { struct { TranslationBlock *tb; } base;
                 bool cpu_has_bps; } DisasContext;
static uint32_t tb_cflags(TranslationBlock *tb) { return tb->cflags; }
'''
defines = set()
for file in ["accel/tcg/tb-jmp-cache.h", "accel/tcg/tb-hash.h",
             "include/exec/translation-block.h"]:
    text = (ROOT / file).read_text()
    for line in re.findall(r"^#define (?:TB_JMP_|CF_)\w+[^\n]*", text, re.M):
        key = line.split()[1]
        if key not in defines:  # First branch is the Emscripten hash layout.
            defines.add(key)
            C += line + "\n"
C += "\n".join(re.findall(r"^#define W_\w+\s+0x[0-9a-f]+", BACK,
                         re.M)) + "\n"
start = HEADER.index("typedef struct WasmLookupLayout")
C += HEADER[start:HEADER.index("} WasmLookupLayout;", start) +
            len("} WasmLookupLayout;")] + "\n"
C += r'''
static const WasmLookupLayout *layout;
static uint32_t marked_flags;
static void tcg_gen_wasm_lookup(const void *p, uint32_t flags) {
    layout = p; marked_flags = flags;
}
static bool capture;
static unsigned nr, nops;
static int64_t ops[128][5];
static void emit(unsigned op, int64_t a, int64_t b, int64_t c, int64_t d) {
    if (capture) {
        assert(nops < 128);
        int64_t v[] = {op, a, b, c, d};
        memcpy(ops[nops++], v, sizeof(v));
    }
}
static void tcg_gen_lookup_and_goto_ptr(void) { emit(12, 0, 0, 0, 0); }
'''
C += function(FRONT, "gen_wasm_lookup_fast")
C += r'''
typedef unsigned TCGv_i32;
typedef unsigned TCGv_ptr;
typedef struct { int id; } TCGLabel;
static TCGLabel miss_label = {0};
static unsigned tcg_env = 0, cpu_eip = 1;
#define TCG_COND_EQ 0
#define TCG_COND_NE 1
static unsigned tcg_temp_new_i32(void) { return nr++; }
static unsigned tcg_temp_new_ptr(void) { return nr++; }
static TCGLabel *gen_new_label(void) { return &miss_label; }
#define tcg_gen_ld_i32(a,b,c) emit(0,a,b,c,0)
#define tcg_gen_ld_ptr tcg_gen_ld_i32
#define tcg_gen_add_i32(a,b,c) emit(1,a,b,c,0)
#define tcg_gen_add_ptr tcg_gen_add_i32
#define tcg_gen_shri_i32(a,b,c) emit(2,a,b,c,0)
#define tcg_gen_xor_i32(a,b,c) emit(3,a,b,c,0)
#define tcg_gen_andi_i32(a,b,c) emit(4,a,b,c,0)
#define tcg_gen_or_i32(a,b,c) emit(5,a,b,c,0)
#define tcg_gen_muli_i32(a,b,c) emit(6,a,b,c,0)
#define tcg_gen_ext_i32_ptr(a,b) emit(7,a,b,0,0)
#define tcg_gen_brcondi_i32(c,a,b,l) emit(8,c,a,b,(l)->id)
#define tcg_gen_brcondi_ptr tcg_gen_brcondi_i32
#define tcg_gen_brcond_i32(c,a,b,l) emit(9,c,a,b,(l)->id)
#define gen_set_label(l) emit(10,(l)->id,0,0,0)
#define tcg_gen_goto_ptr(p) emit(11,p,0,0,0)
static void gen_lookup_phase(const char *name) { }
/* The oracle always emits the original frontend probe, not the new marker. */
#undef CONFIG_TCG_WASM_JIT
'''
C += function(FRONT, "gen_lookup_and_goto_ptr_inline")
C += "#define CONFIG_TCG_WASM_JIT 1\n"
C += function(FRONT, "gen_lookup_and_goto_ptr_inline").replace(
    "gen_lookup_and_goto_ptr_inline", "gen_lookup_marked", 1)
for name in ["wasm_addr", "wasm_lookup_env", "wasm_lookup_fail_if",
             "wasm_lookup_probe"]:
    C += function(BACK, name)
C += r'''
int main(void) {
    bool enabled = gen_wasm_lookup_fast(0);
    const char *e = getenv("XEMU_WASM_LOOKUP_FAST");
    assert(enabled == (e && *e == '1'));
    assert(!!layout == enabled);
    unsigned gates[] = {0, CF_PCREL, CF_USE_ICOUNT, CF_NO_GOTO_PTR,
                        1, CF_NOIRQ, CF_MEMI_ONLY, CF_SINGLE_STEP, CF_BP_PAGE};
    for (unsigned bp = 0; bp < 2; bp++) {
        for (unsigned i = 0; i < sizeof(gates) / sizeof(gates[0]); i++) {
            TranslationBlock tb = {.cflags = gates[i]};
            DisasContext ctx = {.base.tb = &tb, .cpu_has_bps = bp};
            layout = NULL; nr = 2; nops = 0; capture = true;
            gen_lookup_marked(&ctx);
            assert(!!layout == (enabled && !bp && i < 3));
            if (layout) {
                assert(marked_flags == gates[i]);
                assert(nops == 1 && ops[0][0] == 12);
            } else {
                int64_t old[128][5]; unsigned count = nops;
                memcpy(old, ops, sizeof(old)); nr = 2; nops = 0;
                gen_lookup_and_goto_ptr_inline(&ctx);
                assert(count == nops);
                assert(!memcmp(old, ops, nops * sizeof(ops[0])));
            }
        }
    }
    capture = false;
    if (!enabled) { puts("[]"); return 0; }
    assert(gen_wasm_lookup_fast(0));
    puts("[");
    unsigned wanted[] = {0, 0x20000, 0x80200000};
    for (unsigned k = 0; k < 3; k++) {
        TranslationBlock tb = {.cflags = wanted[k]};
        DisasContext ctx = {.base.tb = &tb};
        nr = 2; nops = 0; capture = true;
        gen_lookup_and_goto_ptr_inline(&ctx);
        capture = false;
        TCGContext s = {0};
        wasm_var(&s, W_LOCAL_GET, 0);
        wasm8(&s, W_I64_EXTEND_I32_U);
        wasm_var(&s, W_LOCAL_SET, LREG(TCG_AREG0));
        wasm_lookup_probe(&s, layout, wanted[k]);
        wasm_var(&s, W_LOCAL_GET, L32_1); wasm8(&s, W_END);
        printf("{\"cflags\":%u,\"layout\":[", wanted[k]);
        /* Layout has only 32-bit integer members. */
        for (unsigned i = 0; i < sizeof(*layout) / 4; i++) {
            int32_t v; memcpy(&v, (const char *)layout + 4 * i, 4);
            printf("%s%d", i ? "," : "", v);
        }
        printf("],\"ops\":[");
        for (unsigned i = 0; i < nops; i++) {
            printf("%s[", i ? "," : "");
            for (unsigned j = 0; j < 5; j++) {
                printf("%s%lld", j ? "," : "", (long long)ops[i][j]);
            }
            printf("]");
        }
        printf("],\"wasm\":\"");
        for (unsigned i = 0; i < s.n; i++) { printf("%02x", s.b[i]); }
        printf("\"}%s\n", k == 2 ? "" : ",");
    }
    puts("]");
    return 0;
}
'''

JS = r'''
const fs = require('fs'), assert = require('assert');
const rows = JSON.parse(fs.readFileSync(process.argv[2]));
const leb = x => { const a = []; do { let b = x & 127; x >>>= 7;
  a.push(b | (x ? 128 : 0)); } while (x); return a; };
const vec = a => [...leb(a.length), ...a];
const sec = (id, a) => [id, ...vec(a)];
const memory = new WebAssembly.Memory({initial: 64});
const m = new Uint32Array(memory.buffer);
const rd = a => m[(a >>> 0) / 4] >>> 0;
const wr = (a, v) => m[(a >>> 0) / 4] = v;
let state = 0x19790427;
const rnd = () => { state ^= state << 13; state ^= state >>> 17;
  state ^= state << 5; return state >>> 0; };
let checks = 0;
for (const row of rows) {
  const names = ['bps','jc','eip','cs','eflags','hflags','fmask','gen',
    'size','tb','pc','egen','tcs','tf','tcf','ptr','shift','pmask','amask'];
  const d = Object.fromEntries(names.map((n,i) => [n,row.layout[i]]));
  const body = [2,4,0x7f,19,0x7e,...Buffer.from(row.wasm,'hex')];
  const mod = new WebAssembly.Module(Uint8Array.from([
    0,97,115,109,1,0,0,0,
    ...sec(1,[1,0x60,1,0x7f,1,0x7f]),
    ...sec(2,[1,1,109,1,109,2,0,64]),
    ...sec(3,[1,0]), ...sec(7,[1,1,102,0,0]), ...sec(10,[1,...vec(body)])]));
  const f = new WebAssembly.Instance(mod,{m:{m:memory}}).exports.f;
  function oracle(env) {
    const r = [env,rd(env+d.eip)];
    const label = row.ops.findIndex(op => op[0] === 10);
    for (let pc = 0; pc < row.ops.length; pc++) {
      const [op,a,b,c,e] = row.ops[pc];
      switch (op) {
        case 0: r[a] = rd(r[b]+c); break;
        case 1: r[a] = (r[b]+r[c])>>>0; break;
        case 2: r[a] = r[b]>>>c; break;
        case 3: r[a] = (r[b]^r[c])>>>0; break;
        case 4: r[a] = (r[b]&c)>>>0; break;
        case 5: r[a] = (r[b]|r[c])>>>0; break;
        case 6: r[a] = Math.imul(r[b],c)>>>0; break;
        case 7: r[a] = r[b]; break;
        case 8: if ((r[b]===c) !== !!a) pc=label; break;
        case 9: if ((r[b]===r[c]) !== !!a) pc=label; break;
        case 10: break;
        case 11: return r[a];
        case 12: return 0; // Original helper fallback, not guest execution.
        default: throw Error(op);
      }
    }
    throw Error('no return');
  }
  const env = 8192, jc = 16384, other = 1048576, tb = 3145728;
  function check(want) {
    const expected = oracle(env), actual = f(env)>>>0;
    if (want !== undefined) assert.equal(expected,want>>>0);
    assert.equal(actual,expected); checks++;
  }
  for (let i=0;i<10000;i++) {
    let cs=rnd(), eip=rnd(), flags=rnd(), hf=rnd(), gen=rnd();
    if (i<8) { cs=0xfffffff0; eip=i*8; gen=i<4?0xffffffff:0; }
    wr(env+d.bps,0); wr(env+d.jc,jc); wr(env+d.cs,cs);
    wr(env+d.eip,eip); wr(env+d.eflags,flags); wr(env+d.hflags,hf);
    const pc=(cs+eip)>>>0, h=(pc^(pc>>>d.shift))>>>0;
    const hash=((h>>>d.shift)&d.pmask)|(h&d.amask), entry=jc+hash*d.size;
    wr(jc+d.gen,gen); wr(entry+d.tb,tb); wr(entry+d.pc,pc);
    wr(entry+d.egen,gen); wr(tb+d.tcs,cs);
    const tf=((flags&d.fmask)|hf)>>>0;
    wr(tb+d.tf,tf); wr(tb+d.tcf,row.cflags); wr(tb+d.ptr,0xfedcba98);
    check(0xfedcba98);
    // No import or cache reset between these revocations and revalidations.
    for (const [addr,v] of [[env+d.bps,4],[entry+d.tb,0],
        [entry+d.pc,pc^1],[entry+d.egen,gen^1],[jc+d.gen,gen^1],
        [tb+d.tcs,cs^1],[tb+d.tf,tf^1],
        [tb+d.tcf,row.cflags^0x4000],[tb+d.tcf,row.cflags^0x20000]]) {
      const old=rd(addr); wr(addr,v); check(0); wr(addr,old);
      check(0xfedcba98);
    }
    // Breakpoints inserted after translation; flag and CS changes are live.
    wr(env+d.hflags,hf^1); check(); wr(env+d.hflags,hf);
    for (const bit of [0x100,0x1000,0x2000,0x10000,0x20000,0x40000]) {
      wr(env+d.eflags,flags^bit); check();
    }
    wr(env+d.eflags,flags);
    wr(env+d.cs,cs+1); check(); wr(env+d.cs,cs);
    // Replace the cache, then clear only the new slot (no generation bump).
    const enew=other+hash*d.size;
    wr(other+d.gen,gen); wr(enew+d.tb,tb); wr(enew+d.pc,pc);
    wr(enew+d.egen,gen); wr(env+d.jc,other); check(0xfedcba98);
    wr(enew+d.tb,0); check(0); wr(env+d.jc,jc); check(0xfedcba98);
    // Live code-pointer replacement and zero result take exactly the same path.
    wr(tb+d.ptr,0x80000000); check(0x80000000);
    wr(tb+d.ptr,0); check(0);
    wr(entry+d.tb,0);
  }
}
console.log(`PASS: ${rows.length} layouts/flags, ${checks} native-Wasm probes `+
  'match actual frontend IR; live invalidation, breakpoints, flags and cache');
'''


def main():
    with tempfile.TemporaryDirectory(prefix="test-wasm-lookup-") as tmp:
        tmp = Path(tmp)
        source, exe = tmp / "emit.c", tmp / "emit"
        source.write_text(C)
        rows = []
        for pad, pc64 in [(p, w) for p in [1, 9, 65] for w in [0, 1]]:
            subprocess.run([*shlex.split(os.environ.get("CC", "cc")),
                            "-std=gnu11", "-O2", f"-DPAD={pad}",
                            f"-DPC64={pc64}", str(source),
                            "-o", str(exe)], check=True)
            for policy in [None, "0", "1", "invalid"]:
                env = dict(os.environ)
                env.pop("XEMU_WASM_LOOKUP_FAST", None)
                env.pop("XEMU_WASM_INLINE_LOOKUP", None)
                if policy is not None:
                    env["XEMU_WASM_LOOKUP_FAST"] = policy
                data = json.loads(subprocess.check_output([str(exe)], env=env))
                if policy == "1":
                    rows.extend(data)
                else:
                    assert not data
        fixture = tmp / "fixtures.json"
        fixture.write_text(json.dumps(rows))
        script = tmp / "run.cjs"
        script.write_text(JS)
        subprocess.run([*shlex.split(os.environ.get("NODE", "node")),
                        str(script), str(fixture)], check=True, timeout=90)


if __name__ == "__main__":
    main()
