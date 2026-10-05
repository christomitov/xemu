/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/xemu-wasm-direct-mem-observe.h"
#include "accel/tcg/internal-common.h"
#include "exec/tlb-flags.h"

/* The dispatcher enforces one vCPU execution thread. No suspended direct
 * activation exists: mode2 reference unwinds are explicitly unverified.
 */
static XwmoLog wasm_memory_log;
static CPUArchState *wasm_memory_owner;
static uint32_t wasm_memory_pc;

static void wasm_memory_result(XwmoResult result)
{
    if (result != XWMO_OK) {
        XSTAT_INC(n_direct_mismatch);
        error_report("direct-x86 memory differential mismatch: result=%u "
                     "inv=%" PRIu64 " observed=%u consumed=%u", result,
                     wasm_memory_log.inv, wasm_memory_log.count,
                     wasm_memory_log.cursor);
        abort();
    }
}

void wasm32_direct_mem_begin(CPUArchState *env, uint32_t pc)
{
    wasm_memory_result(xwmo_begin(&wasm_memory_log, wasm_memory_log.inv + 1));
    wasm_memory_owner = env;
    wasm_memory_pc = pc;
}

void wasm32_direct_mem_observe(CPUArchState *env, uint32_t offset,
                              uint32_t addr, uint32_t oi, uint32_t store,
                              uint64_t value)
{
    wasm_memory_result(env == wasm_memory_owner ? XWMO_OK : XWMO_STALE);
    wasm_memory_result(xwmo_observe(&wasm_memory_log, wasm_memory_log.inv,
                       (uint32_t)(wasm_memory_pc + offset), addr, oi,
                       store ? XWMO_STORE : XWMO_LOAD, value));
    XSTAT_INC(n_direct_memory_observed);
}

uint32_t wasm32_direct_mem_load(CPUArchState *env, uint32_t pc,
                                uint32_t addr, uint32_t oi)
{
    uint64_t value = 0;
    wasm_memory_result(env == wasm_memory_owner ? XWMO_OK : XWMO_STALE);
    wasm_memory_result(xwmo_shadow_load(&wasm_memory_log, wasm_memory_log.inv,
                                       pc, addr, oi, &value));
    unsigned bits = xwmo_size_bits(oi);
    return bits < 32 ? value & ((1u << bits) - 1) : value;
}

void wasm32_direct_mem_store(CPUArchState *env, uint32_t pc, uint32_t addr,
                            uint32_t oi, uint32_t value)
{
    wasm_memory_result(env == wasm_memory_owner ? XWMO_OK : XWMO_STALE);
    wasm_memory_result(xwmo_shadow_store(&wasm_memory_log, wasm_memory_log.inv,
                                        pc, addr, oi, value));
}

void wasm32_direct_mem_end(CPUArchState *env)
{
    wasm_memory_result(env == wasm_memory_owner ? XWMO_OK : XWMO_STALE);
    wasm_memory_result(xwmo_end(&wasm_memory_log, wasm_memory_log.inv));
    XSTAT_ADD(n_direct_memory_checked, wasm_memory_log.count);
    XSTAT_INC(n_direct_memory_tb_checked);
}

/* Must remain a call-free leaf: also called on an original Asyncify rewind.
 * Fault/unwind exits do not establish a completed architectural check.
 */
void wasm32_direct_mem_cancel(void)
{
    if (wasm_memory_log.active) {
        xwmo_cancel(&wasm_memory_log, wasm_memory_log.inv);
        xwmo_end(&wasm_memory_log, wasm_memory_log.inv);
        XSTAT_INC(n_direct_memory_unverified);
    }
}

/* No guest access, timing, helper replay or forced IRQ suppression. A plain
 * redispatch could pick the same direct entry and miss forever. COUNT1 is
 * explicitly excluded by the direct frontend, including after code warming.
 */
void wasm32_direct_mem_miss(CPUArchState *env)
{
    CPUState *cpu = env_cpu(env);
    uint32_t flags = cpu->cflags_next_tb;
    if (flags == UINT32_MAX) {
        flags = curr_cflags(cpu);
    }
    cpu->cflags_next_tb = (flags & ~CF_COUNT_MASK) | 1 |
                         CF_NO_GOTO_TB | CF_NO_GOTO_PTR;
    XSTAT_INC(n_direct_memory_miss);
}

/* Diagnostic only. tag is the exact word used by the failed inline probe,
 * not a second TLB read. No guest accesses or effects are replayed here.
 * Load/store counters partition misses; the five reason counters also
 * partition misses (alignment takes priority). notdirty_tag overlaps those
 * reasons and does NOT establish that helper_wasm_notdirty would succeed.
 * Admission currently requires a_mask >= size - 1, so an aligned access
 * cannot span pages. This diagnoses the existing conservative policy only.
 */
void wasm32_direct_mem_miss_classify(CPUArchState *env, uint32_t addr,
                                    uint32_t tag, uint32_t a_mask,
                                    uint32_t store)
{
    uint32_t page = addr & (uint32_t)TARGET_PAGE_MASK;
    bool notdirty = store && tag == (page | TLB_NOTDIRTY);

    if (store) {
        XSTAT_INC(n_direct_miss_store);
    } else {
        XSTAT_INC(n_direct_miss_load);
    }
    if (notdirty) {
        XSTAT_INC(n_direct_miss_notdirty_tag);
    }
    if (addr & a_mask) {
        XSTAT_INC(n_direct_miss_align);
    } else if (notdirty) {
        XSTAT_INC(n_direct_miss_notdirty_aligned);
    } else if ((tag & (uint32_t)TARGET_PAGE_MASK) != page) {
        XSTAT_INC(n_direct_miss_page);
    } else if (tag & ~(uint32_t)TARGET_PAGE_MASK) {
        XSTAT_INC(n_direct_miss_flags);
    } else {
        XSTAT_INC(n_direct_miss_other);
    }
    wasm32_direct_mem_miss(env);
}
