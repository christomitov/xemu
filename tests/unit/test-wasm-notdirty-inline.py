#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Actual-source NOTDIRTY admission with mock CPU/TLB/page/dirty descriptors.

Checks admission/decline, lock balance and unchanged dirty-update calls before
any store. This is not an emulator, a concurrency proof, or a rewind test.
"""
import argparse
import importlib.util
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location(
    'census_fixture', Path(__file__).with_name('test-wasm-census.py'))
census = importlib.util.module_from_spec(spec)
spec.loader.exec_module(census)
function = census.function
TLB = (ROOT / 'accel/tcg/cputlb.c').read_text()
TB = (ROOT / 'accel/tcg/tb-maint.c').read_text()
MEMOP = (ROOT / 'include/exec/memop.h').read_text().replace(
    '#include "qemu/host-utils.h"', '')

PREFIX = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define EMSCRIPTEN 1
#define XBOX 1
#define HOST_BIG_ENDIAN 0
#define CONFIG_SOFTMMU 1
#define TARGET_PAGE_BITS 12
#define TARGET_PAGE_SIZE 4096u
#define TARGET_PAGE_MASK (~4095u)
#define QEMU_BUILD_BUG_ON(x) _Static_assert(!(x), #x)
#define MAX(a,b) ((a) > (b) ? (a) : (b))
static bool is_power_of_2(uint64_t v) { return v && !(v & (v-1)); }
static unsigned clz32(uint32_t v) { return v ? __builtin_clz(v) : 32; }
static unsigned ctz32(uint32_t v) { return v ? __builtin_ctz(v) : 32; }
#include "exec/tlb-flags.h"
#include "qemu/xemu-wasm-stats.h"
XemuWasmStats xemu_wasm_stats;
'''
MOCKS = r'''
#include "exec/memopidx.h"
typedef uint64_t ram_addr_t;
typedef struct { uint32_t addr_write; uintptr_t addend; } CPUTLBEntry;
typedef struct {
    unsigned slow_flags[3];
    uint64_t xlat_section;
} CPUTLBEntryFull;
#define NB_MMU_MODES 3
#define MMU_DATA_STORE 1
#define DIRTY_MEMORY_CODE 3
#define DIRTY_CLIENTS_NOCODE (31u & ~(1u << DIRTY_MEMORY_CODE))
enum { TRACE_MEMORY_NOTDIRTY_WRITE_ACCESS, TRACE_MEMORY_NOTDIRTY_SET_DIRTY };
typedef struct {
    struct { struct { struct { CPUTLBEntryFull fulltlb[4]; }
                      d[NB_MMU_MODES]; } tlb; } neg;
} CPUState;
typedef struct { CPUState *cpu; } CPUArchState;
typedef struct {
    void *first_tb;
    uint8_t *code_bitmap;
    uint32_t smc_miss;
} PageDesc;
static CPUState cpu;
static CPUArchState env = { &cpu };
static CPUTLBEntry entries[NB_MMU_MODES][4];
static struct { unsigned depth; } reader;
static PageDesc page;
static uint8_t bitmap[TARGET_PAGE_SIZE];
static int smc_policy;
static bool missing, busy, locked, plugin, trace_flags[2], xen;
static unsigned locks, unlocks, finds, reads, updates, events, dirty;
static unsigned stores, attempts;
static ram_addr_t want_ram;
static unsigned want_size;
#define get_ptr_rcu_reader() (&reader)
static bool xen_enabled(void) { return xen; }
static CPUState *env_cpu(CPUArchState *e) { assert(e == &env); return e->cpu; }
static unsigned tlb_index(CPUState *c, unsigned mmu, uint32_t addr) {
    assert(c == &cpu && mmu < NB_MMU_MODES);
    return (addr >> TARGET_PAGE_BITS) & 3;
}
static CPUTLBEntry *tlb_entry(CPUState *c, unsigned mmu, uint32_t addr) {
    return &entries[mmu][tlb_index(c, mmu, addr)];
}
static uint32_t tlb_read_idx(CPUTLBEntry *entry, unsigned access) {
    assert(access == MMU_DATA_STORE); return entry->addr_write;
}
static bool cpu_plugin_mem_cbs_enabled(CPUState *c) {
    assert(c == &cpu); return plugin;
}
static bool trace_event_get_state_backends(unsigned event) {
    assert(event < 2); return trace_flags[event];
}
static bool physical_memory_get_dirty_flag(ram_addr_t ram, unsigned client) {
    assert(reader.depth > 0 && ram == want_ram && client == DIRTY_MEMORY_CODE);
    reader.depth++; reads++; reader.depth--;
    return dirty & (1u << client);
}
static void physical_memory_set_dirty_range(ram_addr_t ram, unsigned len,
                                            unsigned mask) {
    assert(reader.depth && !locked && !stores);
    assert(ram == want_ram && len == want_size && mask == DIRTY_CLIENTS_NOCODE);
    reader.depth++; updates++; dirty |= mask; reader.depth--;
}
void xemu_wasm_count(const char *key) {
    assert(!strcmp(key, "invsrc:cpu") && updates == 1 && !stores);
    events++;
}
static PageDesc *page_find(ram_addr_t index) {
    assert(index == want_ram >> TARGET_PAGE_BITS); finds++;
    return missing ? NULL : &page;
}
static bool page_trylock(PageDesc *p) {
    assert(p == &page); locks++;
    if (busy) return true;
    assert(!locked); locked = true; return false;
}
static void page_unlock(PageDesc *p) {
    assert(p == &page && locked); unlocks++; locked = false;
}
static unsigned find_next_bit(const uint8_t *b, unsigned end, unsigned start) {
    assert(locked && b == bitmap && start < end && end <= TARGET_PAGE_SIZE);
    for (unsigned i=start; i<end; i++) if(b[i]) return i;
    return end;
}
'''
MAIN = r'''
static void reset(uint32_t addr, unsigned mmu, unsigned size) {
    memset(&xemu_wasm_stats, 0, sizeof(xemu_wasm_stats));
    memset(&cpu, 0, sizeof(cpu)); memset(entries, 0, sizeof(entries));
    memset(bitmap, 0, sizeof(bitmap));
    page = (PageDesc){
        .first_tb = &cpu, .code_bitmap = bitmap, .smc_miss = 23
    };
    reader.depth = 1; smc_policy = 0;
    missing = busy = locked = plugin = xen = false;
    trace_flags[0] = trace_flags[1] = false;
    locks = unlocks = finds = reads = updates = events = stores = dirty = 0;
    want_ram = 0x44000u + (addr & 4095); want_size = size;
    unsigned i = tlb_index(&cpu, mmu, addr);
    cpu.neg.tlb.d[mmu].fulltlb[i].xlat_section = want_ram - addr;
    entries[mmu][i].addr_write = (addr & TARGET_PAGE_MASK) | TLB_NOTDIRTY;
    entries[mmu][i].addend = (uintptr_t)0x80000000 - addr;
}
static void check(uint32_t addr, unsigned mmu, MemOp op, bool admit) {
    unsigned depth = reader.depth, before = dirty;
#ifdef CONFIG_DEBUG_TCG
    admit = false;
#endif
    uintptr_t host = helper_wasm_notdirty(&env, addr, make_memop_idx(op, mmu));
    attempts++;
    assert(host == (admit ? (uintptr_t)0x80000000 : 0));
    assert(reader.depth == depth && !locked && !stores);
    assert(updates == admit && events == admit);
    assert(dirty == (admit ? (before | DIRTY_CLIENTS_NOCODE) : before));
    assert(page.smc_miss == 23u + admit);
    assert(xemu_wasm_stats.n_smc_bitmap_miss == admit);
    assert(xemu_wasm_stats.n_notdirty_inline == admit);
    assert(xemu_wasm_stats.n_notdirty_inline_miss == !admit);
    assert(unlocks == locks - (busy && locks ? 1 : 0));
#ifdef CONFIG_DEBUG_TCG
    assert(!finds && !locks);
#endif
}
int main(void) {
    /* Actual MemOp encoding: sizes, atomicity, alignment and mmu slots. */
    for (unsigned sz=0; sz<=MO_SIZE; sz++)
    for (unsigned atom=0; atom<8; atom++)
    for (unsigned align=0; align<=7; align++)
    for (unsigned mmu=0; mmu<NB_MMU_MODES; mmu++)
    for (unsigned off=0; off<16; off++) {
        uint32_t addr=0x1234000+off;
        MemOp op=sz | (atom << MO_ATOM_SHIFT) | (align << MO_ASHIFT);
        unsigned ab=MAX(memop_alignment_bits(op), sz);
        bool admit=sz<=MO_32 && (atom==0 || atom==5) && !(addr & ((1u<<ab)-1));
        reset(addr, mmu, 1u<<sz); check(addr, mmu, op, admit);
    }
    for (unsigned d=0; d<32; d++) {
        reset(0x1234030, 0, 4); dirty=d;
        check(0x1234030, 0, MO_32, !(d & (1u << DIRTY_MEMORY_CODE)));
    }
    for (unsigned sz=0; sz<=MO_32; sz++) {
        unsigned len=1u<<sz;
        /* Every byte of a store, including the final RAM-page byte. */
        uint32_t addr=UINT32_MAX-len+1;
        for (unsigned code=4096-len; code<4096; code++) {
            reset(addr, 1, len); bitmap[code]=1;
            check(addr, 1, sz, false);
        }
        reset(addr, 1, len); bitmap[4096-len-1]=1;
        check(addr, 1, sz, true);
    }
    for (unsigned flag=1; flag<512; flag<<=1) {
        reset(0x1234030, 0, 4); entries[0][0].addr_write ^= flag;
        check(0x1234030, 0, MO_32, false);
    }
    for (unsigned flag=1; flag<256; flag<<=1) {
        reset(0x1234030, 0, 4);
        cpu.neg.tlb.d[0].fulltlb[0].slow_flags[MMU_DATA_STORE]=flag;
        check(0x1234030, 0, MO_32, false);
    }
    for (unsigned bit=0; bit<16; bit++) {
        unsigned flag=1u<<bit;
        if (!(flag & ~(MO_SIZE | MO_AMASK | MO_ATOM_MASK))) continue;
        reset(0x1234030, 0, 4); check(0x1234030, 0, MO_32 | flag, false);
    }
    for (int policy=-1; policy<=3; policy++) {
        reset(0x1234030, 0, 4); smc_policy=policy;
        check(0x1234030, 0, MO_32, policy==0);
    }
    for (unsigned mode=0; mode<12; mode++) {
        reset(0x1234030, 0, 4);
        switch(mode) {
        case 0: reader.depth=0; break;
        case 1: missing=true; break;
        case 2: busy=true; break;
        case 3: page.code_bitmap=NULL; break;
        case 4: page.first_tb=NULL; break;
        case 5: plugin=true; break;
        case 6: trace_flags[0]=true; break;
        case 7: trace_flags[1]=true; break;
        case 8: entries[0][0].addend=(uintptr_t)-0x1234030; break;
        case 9: xen=true; break;
        case 10: reader.depth=3; break;
        case 11: cpu.neg.tlb.d[0].fulltlb[0].xlat_section++; want_ram++; break;
        }
        check(0x1234030, 0, MO_32, mode==10);
    }
    for (unsigned mmu=NB_MMU_MODES; mmu<32; mmu++) {
        reset(0x1234030, 0, 4); check(0x1234030, mmu, MO_32, false);
    }
    puts("PASS: leaf/bitmap admission, declines, locks, dirty-call ordering");
    printf("%u probes, debug=%u\n", attempts,
#ifdef CONFIG_DEBUG_TCG
           1u
#else
           0u
#endif
    );
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--wasm', action='store_true')
    args = parser.parse_args()
    code = (PREFIX + MEMOP + MOCKS +
            function(TB, 'tb_wasm_smc_bitmap_miss') +
            function(TLB, 'helper_wasm_notdirty') + MAIN)
    with tempfile.TemporaryDirectory(prefix='wasm-notdirty-inline-') as temp:
        temp = Path(temp)
        source = temp/'test.c'
        source.write_text(code)
        cc = shlex.split(os.environ.get('CC', 'cc'))
        for debug in [0, 1]:
            exe = temp/f'test-{debug}'
            flags = ['-DCONFIG_DEBUG_TCG=1'] if debug else []
            subprocess.run([*cc, '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra',
                            '-Werror', '-Wno-unused-parameter',
                            '-Wno-unused-function',
                            '-fsanitize=address,undefined',
                            '-fno-omit-frame-pointer', '-I'+str(ROOT/'include'),
                            *flags, str(source), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)
            if args.wasm:
                out = temp/f'test-{debug}.js'
                emcc = shlex.split(os.environ.get('EMCC', 'emcc'))
                subprocess.run([*emcc, '-O1', '-sENVIRONMENT=node',
                                '-sINITIAL_MEMORY=32MB', '-sEXIT_RUNTIME=1',
                                '-I'+str(ROOT/'include'), *flags,
                                str(source), '-o', str(out)], check=True)
                subprocess.run([*shlex.split(os.environ.get('NODE', 'node')),
                                str(out)], check=True)


if __name__ == '__main__':
    main()
