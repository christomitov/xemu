#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exact failed-probe tag capture and pure miss classification.

Run the existing memory semantic oracle with diagnostic capture enabled and
mutate the TLB tag AFTER the original probe. The recorded tag must remain the
original one; reference mode must not probe/capture it at all. This is not an
Asyncify, MMIO replay, or end-to-end emulator oracle.
"""
import importlib.util
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def runtime_test():
    source = (ROOT / 'tcg/wasm32-direct-memory-runtime.c.inc').read_text()
    source = source[source.index('void wasm32_direct_mem_miss('):]
    fields = sorted(set(re.findall(r'XSTAT_INC\((\w+)\)', source)))
    prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define TARGET_PAGE_BITS 12
#define TARGET_PAGE_MASK 0xfffff000u
#define QEMU_BUILD_BUG_ON(x) _Static_assert(!(x), #x)
#include "exec/tlb-flags.h"
#define CF_COUNT_MASK 0xffffu
#define CF_NO_GOTO_TB (1u << 20)
#define CF_NO_GOTO_PTR (1u << 21)
typedef struct CPUArchState { uint32_t cflags_next_tb, current; } CPUArchState;
typedef CPUArchState CPUState;
#define env_cpu(e) (e)
static unsigned curr_calls;
static uint32_t curr_cflags(CPUState *c) { curr_calls++; return c->current; }
static struct {
''' + ''.join('uint64_t ' + f + ';\n' for f in fields) + r'''
} stats;
#define XSTAT_INC(f) (stats.f++)
'''
    test = r'''
int main(void) {
    unsigned probes = 0;
    for (unsigned store = 0; store < 2; store++)
    for (unsigned shift = 0; shift < 5; shift++)
    for (unsigned offset = 0; offset < 32; offset++)
    for (unsigned stale = 0; stale < 2; stale++)
    for (unsigned f = 0; f < 4096; f++) {
        uint32_t addr = 0xfffff000u + offset;
        uint32_t tag = (stale ? 0x123000u : 0xfffff000u) | f;
        uint32_t mask = (1u << shift) - 1;
        CPUState cpu = { (probes & 1) ? UINT32_MAX : 0x87654321u,
                         0x43218765u };
        uint32_t old = cpu.cflags_next_tb == UINT32_MAX ? cpu.current :
                       cpu.cflags_next_tb;
        bool expected_current = cpu.cflags_next_tb == UINT32_MAX;
        curr_calls = 0;
        memset(&stats, 0, sizeof(stats));
        wasm32_direct_mem_miss_classify(&cpu, addr, tag, mask, store);
        assert(cpu.cflags_next_tb == ((old & ~CF_COUNT_MASK) | 1 |
                                     CF_NO_GOTO_TB | CF_NO_GOTO_PTR));
        assert(curr_calls == expected_current);
        assert(stats.n_direct_memory_miss == 1);
        assert(stats.n_direct_miss_load == !store);
        assert(stats.n_direct_miss_store == store);
        assert(stats.n_direct_miss_notdirty_tag ==
               (store && !stale && f == TLB_NOTDIRTY));
        /* Independent categorical oracle, using offset/fixture flags. */
        bool align = offset % (1u << shift) != 0;
        bool nd = !align && store && !stale && f == TLB_NOTDIRTY;
        assert(stats.n_direct_miss_align == align);
        assert(stats.n_direct_miss_notdirty_aligned == nd);
        assert(stats.n_direct_miss_page == (!align && !nd && stale));
        assert(stats.n_direct_miss_flags ==
               (!align && !nd && !stale && f != 0));
        assert(stats.n_direct_miss_other ==
               (!align && !nd && !stale && f == 0));
        probes++;
    }
    printf("PASS %u classifier/canonical-request probes\n", probes);
}
'''
    with tempfile.TemporaryDirectory(prefix='xwd-miss-runtime-') as tmp:
        tmp = Path(tmp)
        (tmp / 'test.c').write_text(prefix + source + test)
        subprocess.run([*shlex.split(os.environ.get('CC', 'cc')), '-O1',
                        '-I' + str(ROOT / 'include'), str(tmp / 'test.c'),
                        '-o', str(tmp / 'test')], check=True)
        subprocess.run([str(tmp / 'test')], check=True)


def changed(text, old, new):
    assert text.count(old) == 1, old
    return text.replace(old, new)


def capture_test():
    spec = importlib.util.spec_from_file_location(
        'memory', Path(__file__).with_name('test-wasm-direct-mem.py'))
    mem = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mem)
    mem.C = changed(mem.C, 'static const XwdMemHooks hooks = {',
                    'static const XwdMemHooks hooks = { .tag_local = XD_NLOCALS,')
    mem.C = changed(mem.C,
                    'assert(xwd_emit_ex(&p,&layout,verify,8192,0,&hooks,&code));',
                    '''assert(xwd_emit_ex(&p,&layout,verify,8192,0,&hooks,&code));
   /* Fixture owns one extra diagnostic local beyond the ordinary body. */
   if (p.accesses) code.bytes[1]++;''')
    mem.C = changed(mem.C,
                    '(void)a; xwd_byte(e, 0x01); /* nop */ return true;',
                    '''/* Change the live tag after its original comparison. */
 xwd_local(e, 0x20, XD_MENTRY); xwd_const(e, 0xbad000);
 xwd_memop(e, 0x36, 2, a->cmp_ofs);
 return true;''')
    mem.C = changed(mem.C,
                    'static bool mock_slow(XwdEmitter *e, const XwdMemAccess *a, unsigned al,\n'
                    '                      unsigned vl)\n{',
                    '''static bool mock_slow(XwdEmitter *e, const XwdMemAccess *a, unsigned al,
                      unsigned vl)
{
 xwd_const(e, ''' + str(mem.SLOW_FLAG + 16) + ''');
 xwd_local(e, 0x20, e->mem->tag_local); xwd_memop(e, 0x36, 2, 0);''')
    mem.JS = changed(mem.JS,
                     'const slowRan=v.getUint32(M.SLOW,true), shadowRan=v.getUint32(M.SHADOW,true);',
                     '''const slowRan=v.getUint32(M.SLOW,true), shadowRan=v.getUint32(M.SHADOW,true);
   assert.equal(v.getUint32(M.SLOW+16,true), fast||verify ? 0x5a5a5a5a :
                mode==='miss' ? ((M.PAGE|0x20)>>>0) : M.PAGE,
                'original probe tag '+mode+' '+c.hex);''')
    mem.main()


if __name__ == '__main__':
    runtime_test()
    capture_test()
