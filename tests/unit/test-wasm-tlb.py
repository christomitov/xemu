#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Execute the production inline TLB emitter in native WebAssembly."""

import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SOURCE = (ROOT / "tcg/wasm32/tcg-target.c.inc").read_text()


def function(name):
    match = re.search(r"(?m)^static (?:void|bool) " + name +
                      r"\([^;]*?\)\n\{", SOURCE)
    assert match, name
    pos, depth = match.end(), 1
    while depth:
        depth += (SOURCE[pos] == "{") - (SOURCE[pos] == "}")
        pos += 1
    return SOURCE[match.start():pos]


C = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#define EMSCRIPTEN 1
#define TARGET_PAGE_BITS 12
#define TARGET_PAGE_MASK (~4095u)
#define CPU_TLB_ENTRY_BITS 4
#define TCG_AREG0 14
#define QEMU_BUILD_BUG_ON(x) _Static_assert(!(x), #x)
typedef uint32_t vaddr;
typedef unsigned TCGReg;
typedef unsigned MemOp;
typedef unsigned MemOpIdx;
typedef struct { unsigned align; } TCGAtomAlign;
typedef struct { uint32_t mask, table; } CPUTLBDescFast;
typedef struct {
    uint32_t addr_read, addr_write, addr_code, addend;
} CPUTLBEntry;
typedef struct { unsigned n; unsigned char b[2048]; } TCGContext;
enum { MO_SIZE = 7, MO_BSWAP = 8, MO_ATOM_IFALIGN = 0 };
enum { L32_0 = 1, L32_1, L32_2, L32_3 };
#define LREG(r) (8 + (r))
enum { W_LOCAL_GET = 0x20, W_LOCAL_SET, W_LOCAL_TEE,
       W_I32_LOAD = 0x28, W_I32_ADD = 0x6a, W_I32_AND = 0x71,
       W_I32_XOR = 0x73, W_I32_SHR_U = 0x76, W_I32_EQ = 0x46,
       W_TYPE_I32 = 0x7f, W_ELSE = 5, W_END = 11 };
static MemOp get_memop(MemOpIdx oi) { return oi & 255; }
static unsigned get_mmuidx(MemOpIdx oi) { return oi >> 8; }
static TCGAtomAlign atom_and_align_for_opc(TCGContext *s, MemOp m,
                                         int atom, bool st) {
    return (TCGAtomAlign){m >> 4};
}
static int tlb_mask_table_ofs(TCGContext *s, unsigned mmu) {
    return -64 + 8 * mmu;
}
static void wasm8(TCGContext *s, unsigned b) {
    assert(s->n < sizeof(s->b)); s->b[s->n++] = b;
}
static void uleb(TCGContext *s, uint32_t v) {
    do {
        unsigned b = v & 127; v >>= 7; wasm8(s, b | (v ? 128 : 0));
    } while (v);
}
static void wasm_var(TCGContext *s, unsigned op, unsigned v) {
    wasm8(s, op); uleb(s, v);
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
    wasm8(s, op); uleb(s, 2); uleb(s, offset);
}
static void wasm_get_r32(TCGContext *s, TCGReg r) {
    wasm_var(s, W_LOCAL_GET, LREG(r)); wasm8(s, 0xa7);
}
static void wasm_if_hint(TCGContext *s, unsigned type, bool hot) {
    wasm8(s, 4); wasm8(s, type);
}
'''
C += function("wasm_tlb_fast") + "\n" + function("wasm_tlb_load") + "\n"
C += r'''
int main(int argc, char **argv) {
    assert(argc == 2); setenv("XEMU_WASM_TLB_FAST", argv[1], 1);
    for (unsigned mmu = 0; mmu < 2; mmu++) {
        for (unsigned ld = 0; ld < 2; ld++) {
            for (unsigned size = 0; size < 4; size++) {
                for (unsigned align = 0; align <= 5; align++) {
                    for (unsigned swap = 0; swap < 2; swap++) {
                        TCGContext s = {0};
                        /* ctx.env/addr -> production i64 locals */
                        wasm_var(&s, W_LOCAL_GET, 0);
                        wasm_mem(&s, W_I32_LOAD, 0); wasm8(&s, 0xad);
                        wasm_var(&s, W_LOCAL_SET, LREG(TCG_AREG0));
                        wasm_var(&s, W_LOCAL_GET, 0);
                        wasm_mem(&s, W_I32_LOAD, 4); wasm8(&s, 0xad);
                        wasm_var(&s, W_LOCAL_SET, LREG(0));
                        wasm_tlb_load(&s, 0, mmu << 8 | align << 4 |
                                     swap * MO_BSWAP | size, ld);
                        /* A helper may replace any mutable TLB field. */
                        wasm_var(&s, 0x10, 0);
                        wasm_tlb_load(&s, 0, mmu << 8 | align << 4 |
                                     swap * MO_BSWAP | size, ld);
                        wasm_var(&s, W_LOCAL_GET, L32_1);
                        wasm8(&s, W_END);
                        printf("%u %u %u %u %u ", mmu, ld, size, align, swap);
                        for (unsigned j = 0; j < s.n; j++) {
                            printf("%02x", s.b[j]);
                        }
                        puts("");
                    }
                }
            }
        }
    }
}
'''


def leb(n):
    out = bytearray()
    while True:
        b = n & 127
        n >>= 7
        out.append(b | (128 if n else 0))
        if not n:
            return bytes(out)


def section(n, b):
    return bytes([n]) + leb(len(b)) + b


def name(s):
    b = s.encode()
    return leb(len(b)) + b


def module(bodies):
    result = b"\0asm\x01\0\0\0"
    result += section(1, b"\x02\x60\x01\x7f\x01\x7f\x60\0\0")
    result += section(2, b"\x02" + name("env") + name("memory") +
                      b"\x02\x00\x20" + name("env") + name("mutate") +
                      b"\0\x01")  # memory >= 32 pages, plus callback
    result += section(3, leb(len(bodies)) + b"\0" * len(bodies))
    exports = leb(len(bodies))
    for i in range(len(bodies)):
        exports += name("p" + str(i)) + b"\0" + leb(i + 1)
    result += section(7, exports)
    code = leb(len(bodies))
    for b in bodies:
        # Original production scratch/register local indices are unchanged.
        b = b"\x04\x04\x7f\x02\x7e\x01\x7c\x11\x7e" + b
        code += leb(len(b)) + b
    return result + section(10, code)


JS = r'''
const assert = require('node:assert/strict');
const fs = require('node:fs');
const meta = JSON.parse(fs.readFileSync(process.argv[2]));
const memory = new WebAssembly.Memory({initial: 32});
const view = new DataView(memory.buffer);
let mutator = null;
const mutate = () => { if (mutator) mutator(); };
const instances = process.argv.slice(3).map(p =>
    new WebAssembly.Instance(new WebAssembly.Module(fs.readFileSync(p)),
                             {env: {memory, mutate}}).exports);
const env = 1024, ctx = 128, host = 0x100000;
view.setUint32(ctx, env, true);
let checks = 0, seed = 0x791214ab;
function random() {
    seed ^= seed << 13; seed ^= seed >>> 17; seed ^= seed << 5;
    return seed >>> 0;
}
function probe(i, addr, entries, table, flags = 0, foreign = false) {
    const [mmu, ld, size, align, swap] = meta[i];
    const mask = (entries - 1) << 4, desc = env - 64 + 8 * mmu;
    const idx = (((addr ^ ((addr >>> 12) & 0xff000)) >>> 8) & mask);
    const page = addr & 0xfffff000;
    const tag = (page ^ (foreign ? 0x1000 : 0)) | flags;
    view.setUint32(desc, mask, true); view.setUint32(desc + 4, table, true);
    view.setUint32(table + idx, ld ? tag : 0x87654321, true);
    view.setUint32(table + idx + 4, ld ? 0x87654321 : tag, true);
    view.setUint32(table + idx + 12, (host - page) >>> 0, true);
    view.setUint32(ctx + 4, addr, true);
    const a = (1 << align) - 1, s = (1 << size) - 1;
    const cmp = ((addr + Math.max(s - a, 0)) >>> 0) & (0xfffff000 | a);
    const expected = !swap && tag === cmp ? host + (addr & 4095) : 0;
    for (const e of instances) {
        assert.equal(e['p' + i](ctx) >>> 0, expected,
                     `probe ${meta[i]} addr=${addr.toString(16)} mask=${mask}`);
        checks++;
    }
}
for (let i = 0; i < meta.length; i++) {
    for (const entries of [64, 256, 1024, 4096]) {
        for (const page of [0, 0x12345000, 0x80000000, 0xfffff000]) {
            for (const off of [0, 1, 2, 3, 15, 31, 4080, 4088, 4092, 4095]) {
                const addr = (page + off) >>> 0;
                probe(i, addr, entries, 0x2000);
                probe(i, addr, entries, 0x18000, 0, true);
                for (const flag of [32, 64, 128, 256, 512, 1024, 2048]) {
                    probe(i, addr, entries, 0x2000, flag);
                }
            }
        }
    }
    // Change descriptor mask/table, tag and addend between executions of the
    // same compiled function: none of these mutable values may be cached.
    for (let j = 0; j < 100; j++) {
        probe(i, random(), 1 << (6 + j % 7), j & 1 ? 0x18000 : 0x2000);
    }
}
// Replace mask, table, tag and addend BETWEEN accesses in one activation.
for (const e of instances) {
    const i = meta.findIndex(m => m.join() === '0,1,2,0,0');
    const addr = 0x12345678, page = addr & 0xfffff000, desc = env - 64;
    const hash = (addr ^ ((addr >>> 12) & 0xff000)) >>> 8;
    for (const flags of [0, 512, 1024]) {
        view.setUint32(ctx + 4, addr, true);
        view.setUint32(desc, 63 << 4, true);
        view.setUint32(desc + 4, 0x2000, true);
        view.setUint32(0x2000 + (hash & (63 << 4)), page, true);
        view.setUint32(0x200c + (hash & (63 << 4)), host - page, true);
        mutator = () => {
            view.setUint32(desc, 4095 << 4, true);
            view.setUint32(desc + 4, 0x18000, true);
            const entry = 0x18000 + (hash & (4095 << 4));
            view.setUint32(entry, page | flags, true);
            view.setUint32(entry + 12, host + 4096 - page, true);
        };
        assert.equal(e['p' + i](ctx), flags ? 0 : host + 4096 + 0x678);
    }
}
mutator = null;
// Independent hash equivalence over every low address bit and many high bits,
// including all entry-aligned masks rather than just power-of-two sizes.
for (let i = 0; i < 200000; i++) {
    const addr = random(), mask = random() & ~15;
    assert.equal((((addr ^ ((addr >>> 12) & 0xff000)) >>> 8) & mask) >>> 0,
                 (((addr ^ (addr >>> 12)) >>> 8) & mask) >>> 0);
}
console.log('PASS:', checks,
            'native TLB probes, live replacements, hash identity');
'''

with tempfile.TemporaryDirectory(prefix="test-wasm-tlb-") as directory:
    p = Path(directory)
    (p / "emit.c").write_text(C)
    (p / "test.cjs").write_text(JS)
    subprocess.run(shlex.split(os.environ.get("CC", "cc")) +
                   ["-std=gnu11", "-O2", str(p / "emit.c"),
                    "-o", str(p / "emit")], check=True, timeout=30)
    variants = []
    for enabled in [0, 1]:
        lines = subprocess.check_output([str(p / "emit"), str(enabled)],
                                        text=True, timeout=10).splitlines()
        meta = [[int(v) for v in line.split()[:5]] for line in lines]
        bodies = [bytes.fromhex(line.split()[5]) for line in lines]
        variants.append(bodies)
        (p / f"{enabled}.wasm").write_bytes(module(bodies))
    for config, off, on in zip(meta, *variants):
        if config[4]:
            assert off == on, "byte-swap fallback changed"
        else:
            assert len(on) < len(off), "lookup did not get smaller"
    (p / "meta.json").write_text(json.dumps(meta))
    subprocess.run(shlex.split(os.environ.get("NODE", "node")) +
                   [str(p / "test.cjs"), str(p / "meta.json"),
                    str(p / "0.wasm"), str(p / "1.wasm")],
                   check=True, timeout=45)
