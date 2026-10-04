/*
 * Single-access memory observer/verifier for the direct x86 translator's
 * mode 2 (differential) checks.
 *
 * A guest memory access must happen exactly once: reading shared RAM or MMIO
 * a second time, or replaying a store, could change what the guest sees. So
 * the shadow (direct) computation never touches guest memory. Instead the
 * original TCG execution reports every access it performs, once, in order:
 *
 *   original:  xwmo_observe(log, inv, pc, addr, oi, kind, value)
 *   shadow:    xwmo_shadow_load(...)  -> the original's loaded value
 *              xwmo_shadow_store(...) -> compares the shadow's store operand
 *   end:       xwmo_end(log, inv)     -> every observed access consumed
 *
 * The shadow consumes the recorded accesses in order and must predict the
 * same PC, address, MemOpIdx and kind for each. A load gets the original's
 * value, normalized to the access size and signedness, so its result can be
 * checked without a second read. A store compares only the bits the access
 * writes. An invocation that faulted or was cancelled is never "completed":
 * its result is XWMO_UNVERIFIED, not a pass.
 *
 * Pure bookkeeping: no guest reads or stores, callbacks, clocks or replay.
 * Where the hooks go, and the bank/TCG suspend/rewind state, belong to the
 * translator.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef QEMU_XEMU_WASM_DIRECT_MEM_OBSERVE_H
#define QEMU_XEMU_WASM_DIRECT_MEM_OBSERVE_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* MemOpIdx = (MemOp << 5) | mmu_idx; MemOp size/sign bits (exec/memop.h) */
#define XWMO_OI_MEMOP_SHIFT 5
#define XWMO_MO_SIZE        0x07
#define XWMO_MO_SIGN        0x08

#ifndef XWMO_MAX_ACCESSES
#define XWMO_MAX_ACCESSES   64  /* per invocation (one TB / region entry) */
#endif

typedef enum XwmoKind {
    XWMO_LOAD = 1,
    XWMO_STORE = 2,
} XwmoKind;

typedef enum XwmoResult {
    XWMO_OK = 0,
    XWMO_UNVERIFIED,        /* invocation faulted/cancelled: not checked */
    XWMO_STALE,             /* invocation id duplicate, old or mismatched */
    XWMO_OVERFLOW,          /* more than XWMO_MAX_ACCESSES observed */
    XWMO_MISSING,           /* shadow expects an access the original lacked */
    XWMO_EXTRA,             /* original made accesses the shadow didn't */
    XWMO_KIND,              /* load vs store */
    XWMO_PC,
    XWMO_ADDR,
    XWMO_OI,
    XWMO_VALUE,             /* store operand differs in the written bits */
    XWMO_BAD_OI,            /* access size beyond 64 bits */
} XwmoResult;

typedef struct XwmoAccess {
    uint64_t pc;
    uint64_t addr;
    uint64_t value;         /* normalized: see xwmo_normalize() */
    uint32_t oi;
    uint8_t kind;
} XwmoAccess;

typedef struct XwmoLog {
    uint32_t inv;           /* current invocation id; 0 = none yet */
    uint32_t count;         /* accesses observed this invocation */
    uint32_t cursor;        /* next access the shadow consumes */
    bool active;            /* between begin and end */
    bool cancelled;         /* faulted / cancelled: result UNVERIFIED */
    XwmoResult error;       /* first error this invocation (sticky) */
    XwmoAccess a[XWMO_MAX_ACCESSES];
} XwmoLog;

static inline unsigned xwmo_size_bits(uint32_t oi)
{
    return 8u << ((oi >> XWMO_OI_MEMOP_SHIFT) & XWMO_MO_SIZE);
}

/*
 * The value of an access as the guest sees it: truncated to the access size,
 * then sign- or zero-extended as its MemOp says. Loads are normalized so the
 * original's helper result and the shadow's view agree however each extends.
 */
static inline uint64_t xwmo_normalize(uint32_t oi, uint64_t value)
{
    unsigned bits = xwmo_size_bits(oi);

    if (bits >= 64) {
        return value;
    }
    value &= (UINT64_C(1) << bits) - 1;
    if (((oi >> XWMO_OI_MEMOP_SHIFT) & XWMO_MO_SIGN) &&
        (value >> (bits - 1)) & 1) {
        value |= ~((UINT64_C(1) << bits) - 1);
    }
    return value;
}

static inline void xwmo_fail(XwmoLog *log, XwmoResult r)
{
    if (log->error == XWMO_OK) {
        log->error = r;
    }
}

/* Start invocation @inv. Ids must strictly increase (0 is never valid). */
static inline XwmoResult xwmo_begin(XwmoLog *log, uint32_t inv)
{
    if (inv == 0 || inv <= log->inv) {
        log->active = false;
        return XWMO_STALE;
    }
    log->inv = inv;
    log->count = log->cursor = 0;
    log->active = true;
    log->cancelled = false;
    log->error = XWMO_OK;
    return XWMO_OK;
}

/* The original completed one access (a load's @value is what it returned). */
static inline XwmoResult xwmo_observe(XwmoLog *log, uint32_t inv, uint64_t pc,
                                      uint64_t addr, uint32_t oi,
                                      XwmoKind kind, uint64_t value)
{
    if (!log->active || inv != log->inv) {
        return XWMO_STALE;
    }
    if (xwmo_size_bits(oi) > 64) {
        xwmo_fail(log, XWMO_BAD_OI);
        return XWMO_BAD_OI;
    }
    if (log->count >= XWMO_MAX_ACCESSES) {
        xwmo_fail(log, XWMO_OVERFLOW);
        return XWMO_OVERFLOW;
    }
    log->a[log->count++] = (XwmoAccess){
        .pc = pc, .addr = addr, .oi = oi, .kind = kind,
        .value = xwmo_normalize(oi, value),
    };
    return XWMO_OK;
}

/* The invocation faulted or was cancelled: nothing it did can be verified. */
static inline void xwmo_cancel(XwmoLog *log, uint32_t inv)
{
    if (log->active && inv == log->inv) {
        log->cancelled = true;
    }
}

static inline XwmoResult xwmo_take(XwmoLog *log, uint32_t inv, uint64_t pc,
                                   uint64_t addr, uint32_t oi, XwmoKind kind,
                                   const XwmoAccess **out)
{
    const XwmoAccess *a;

    if (!log->active || inv != log->inv) {
        return XWMO_STALE;
    }
    if (log->cancelled) {
        return XWMO_UNVERIFIED;
    }
    if (log->cursor >= log->count) {
        xwmo_fail(log, XWMO_MISSING);
        return XWMO_MISSING;
    }
    a = &log->a[log->cursor++];
    if (a->kind != kind) {
        xwmo_fail(log, XWMO_KIND);
        return XWMO_KIND;
    }
    if (a->pc != pc) {
        xwmo_fail(log, XWMO_PC);
        return XWMO_PC;
    }
    if (a->addr != addr) {
        xwmo_fail(log, XWMO_ADDR);
        return XWMO_ADDR;
    }
    if (a->oi != oi) {
        xwmo_fail(log, XWMO_OI);
        return XWMO_OI;
    }
    *out = a;
    return XWMO_OK;
}

/* Shadow load: on XWMO_OK, *value is the original's normalized result. */
static inline XwmoResult xwmo_shadow_load(XwmoLog *log, uint32_t inv,
                                          uint64_t pc, uint64_t addr,
                                          uint32_t oi, uint64_t *value)
{
    const XwmoAccess *a;
    XwmoResult r = xwmo_take(log, inv, pc, addr, oi, XWMO_LOAD, &a);

    if (r == XWMO_OK) {
        *value = a->value;
    }
    return r;
}

/* Shadow store: compares the bits the access writes. */
static inline XwmoResult xwmo_shadow_store(XwmoLog *log, uint32_t inv,
                                           uint64_t pc, uint64_t addr,
                                           uint32_t oi, uint64_t value)
{
    const XwmoAccess *a;
    XwmoResult r = xwmo_take(log, inv, pc, addr, oi, XWMO_STORE, &a);

    if (r == XWMO_OK && a->value != xwmo_normalize(oi, value)) {
        xwmo_fail(log, XWMO_VALUE);
        return XWMO_VALUE;
    }
    return r;
}

/*
 * End invocation @inv: the first error, else UNVERIFIED if cancelled, else
 * EXTRA if the original made accesses the shadow never consumed, else OK.
 */
static inline XwmoResult xwmo_end(XwmoLog *log, uint32_t inv)
{
    XwmoResult r;

    if (!log->active || inv != log->inv) {
        return XWMO_STALE;
    }
    log->active = false;
    if (log->error != XWMO_OK) {
        r = log->error;
    } else if (log->cancelled) {
        r = XWMO_UNVERIFIED;
    } else if (log->cursor != log->count) {
        r = XWMO_EXTRA;
    } else {
        r = XWMO_OK;
    }
    return r;
}

#endif /* QEMU_XEMU_WASM_DIRECT_MEM_OBSERVE_H */
