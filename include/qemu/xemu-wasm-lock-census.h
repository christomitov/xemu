/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef QEMU_XEMU_WASM_LOCK_CENSUS_H
#define QEMU_XEMU_WASM_LOCK_CENSUS_H

/* Default-off diagnostics. Caller census adds no clocks or lock probes.
 * BQL_DETAIL additionally times main-loop phases while BQL is held.
 */
enum XemuWasmLockKind {
    XWLC_LOCK,
    XWLC_TRY,
    XWLC_BQL,
    XWLC_BQL_WAIT,
};

enum XemuWasmBqlDetail {
    XWLD_OUTER, XWLD_SETUP, XWLD_GLIB_PREPARE, XWLD_GLIB_CHECK,
    XWLD_GLIB_DISPATCH, XWLD_NOTIFY, XWLD_DEADLINE, XWLD_TIMERS,
    XWLD_TIMER_CB, XWLD_BH_CB, XWLD_FD_READ, XWLD_FD_WRITE,
    XWLD_FD_POLL,
};

#ifdef EMSCRIPTEN
#include "qemu/atomic.h"
extern bool xemu_wasm_lock_census_enabled;
extern bool xemu_wasm_lock_calls_enabled;
extern bool xemu_wasm_bql_detail_enabled;
extern __thread int xemu_wasm_is_vcpu;
void xemu_wasm_lock_census_init(void);
unsigned xemu_wasm_bql_detail_begin(unsigned kind, uint32_t id);
void xemu_wasm_bql_detail_end(unsigned token);
unsigned xemu_wasm_bql_detail_main(void);

#define XWLD_BEGIN(k, id) (qatomic_read(&xemu_wasm_bql_detail_enabled) ? \
    xemu_wasm_bql_detail_begin(k, (uint32_t)(uintptr_t)(id)) : 0)
#define XWLD_END(t) do { if (t) { xemu_wasm_bql_detail_end(t); } } while (0)
#define XWLD_MAIN() (qatomic_read(&xemu_wasm_bql_detail_enabled) ? \
    xemu_wasm_bql_detail_main() : 0)
void xemu_wasm_lock_census_note(unsigned kind, const char *file, int line,
                                bool busy);
void xemu_wasm_lock_census_publish(const char *file, int line);
uint64_t xemu_wasm_lock_census_holder(void);
void xemu_wasm_lock_census_wait(const char *file, int line, uint64_t holder,
                               int64_t ns);

#define XWLC_HOLDER(file, line) do { \
    if (unlikely(qatomic_read(&xemu_wasm_lock_census_enabled))) { \
        xemu_wasm_lock_census_publish(file, line); \
    } \
} while (0)
#define XWLC_MUTEX_HOLDER(m, file, line) do { \
    if (unlikely(qatomic_read(&xemu_wasm_lock_census_enabled)) && \
        mutex_is_bql(m)) { \
        xemu_wasm_lock_census_publish(file, line); \
    } \
} while (0)

#define XWLC_NOTE(kind, file, line, busy) do { \
    if (unlikely(qatomic_read(&xemu_wasm_lock_census_enabled) && \
                 qatomic_read(&xemu_wasm_lock_calls_enabled) && \
                 xemu_wasm_is_vcpu)) { \
        xemu_wasm_lock_census_note(kind, file, line, busy); \
    } \
} while (0)
#else
#define XWLD_BEGIN(k, id) 0
#define XWLD_END(t) ((void)(t))
#define XWLD_MAIN() 0
#define XWLC_HOLDER(file, line) do { } while (0)
#define XWLC_MUTEX_HOLDER(m, file, line) do { } while (0)
#define XWLC_NOTE(kind, file, line, busy) do { } while (0)
#endif
#endif
