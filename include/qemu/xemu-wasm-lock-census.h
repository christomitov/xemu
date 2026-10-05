/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef QEMU_XEMU_WASM_LOCK_CENSUS_H
#define QEMU_XEMU_WASM_LOCK_CENSUS_H

/* Default-off, vCPU-thread-only diagnostic. No clocks or extra lock probes. */
enum XemuWasmLockKind {
    XWLC_LOCK,
    XWLC_TRY,
    XWLC_BQL,
    XWLC_BQL_WAIT,
};

#ifdef EMSCRIPTEN
#include "qemu/atomic.h"
extern bool xemu_wasm_lock_census_enabled;
extern __thread int xemu_wasm_is_vcpu;
void xemu_wasm_lock_census_init(void);
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
                 xemu_wasm_is_vcpu)) { \
        xemu_wasm_lock_census_note(kind, file, line, busy); \
    } \
} while (0)
#else
#define XWLC_HOLDER(file, line) do { } while (0)
#define XWLC_MUTEX_HOLDER(m, file, line) do { } while (0)
#define XWLC_NOTE(kind, file, line, busy) do { } while (0)
#endif
#endif
