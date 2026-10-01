/*
 * The per-CPU TranslationBlock jump cache.
 *
 *  Copyright (c) 2003 Fabrice Bellard
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef ACCEL_TCG_TB_JMP_CACHE_H
#define ACCEL_TCG_TB_JMP_CACHE_H

#include "qemu/rcu.h"
#include "exec/cpu-common.h"

#define TB_JMP_CACHE_BITS 15
#define TB_JMP_CACHE_SIZE (1 << TB_JMP_CACHE_BITS)

/*
 * Invalidated in parallel; all accesses to 'tb' must be atomic.
 * A valid entry is read/written by a single CPU, therefore there is
 * no need for qatomic_rcu_read() and pc is always consistent with a
 * non-NULL value of 'tb'.  Strictly speaking pc is only needed for
 * CF_PCREL, but it's used always for simplicity.
 */
typedef struct CPUJumpCache {
    struct rcu_head rcu;
#ifdef EMSCRIPTEN
    /*
     * Generation: an entry is valid only if its gen matches, so a flush
     * bumps this instead of clearing 32K entries (the Xbox reloads CR3
     * ~450 times/s, each a full TLB + jump cache flush).
     */
    uint32_t gen;
#endif
    struct {
        TranslationBlock *tb;
        vaddr pc;
#ifdef EMSCRIPTEN
        uint32_t gen;
#endif
    } array[TB_JMP_CACHE_SIZE];
} CPUJumpCache;

static inline bool tb_jmp_cache_match(const CPUJumpCache *jc, uint32_t h,
                                      vaddr pc)
{
#ifdef EMSCRIPTEN
    return jc->array[h].pc == pc && jc->array[h].gen == jc->gen;
#else
    return jc->array[h].pc == pc;
#endif
}

static inline void tb_jmp_cache_set_pc(CPUJumpCache *jc, uint32_t h,
                                       vaddr pc)
{
    jc->array[h].pc = pc;
#ifdef EMSCRIPTEN
    jc->array[h].gen = jc->gen;
#endif
}

#endif /* ACCEL_TCG_TB_JMP_CACHE_H */
