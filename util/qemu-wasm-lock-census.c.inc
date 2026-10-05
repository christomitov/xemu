/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Included by qemu-thread-posix.c on Wasm only. */
#include <emscripten/emscripten.h>

#define XWLC_SLOTS 128
static struct {
    const char *file;                /* published last, immutable thereafter */
    unsigned kind;
    int line;
    uint32_t calls, busy;
} xwlc_sites[XWLC_SLOTS];
static uint32_t xwlc_overflow;
bool xemu_wasm_lock_census_enabled;

void xemu_wasm_lock_census_init(void)
{
    const char *e = getenv("XEMU_WASM_LOCK_CENSUS");
    /* Called at vCPU thread startup, before it can record anything. */
    qatomic_set(&xemu_wasm_lock_census_enabled, e && *e == '1');
}

void xemu_wasm_lock_census_note(unsigned kind, const char *file, int line,
                                bool busy)
{
    unsigned hash = ((uintptr_t)file >> 3) ^ (line * 31u) ^ (kind * 13u);

    /* Single writer, no allocation/interning, no mutexes and no clocks.
     * Atomic counters/publication allow the reporting thread to snapshot.
     * Overflow includes saturated counters; never silently call it zero.
     */
    for (unsigned i = 0; i < XWLC_SLOTS; i++) {
        unsigned slot = (hash + i) & (XWLC_SLOTS - 1);
        typeof(xwlc_sites[0]) *s = &xwlc_sites[slot];
        if (!s->file) {
            s->kind = kind;
            s->line = line;
            qatomic_store_release(&s->file, file);
        }
        if (s->file == file && s->line == line && s->kind == kind) {
            uint32_t calls = qatomic_read(&s->calls);
            if (calls == UINT32_MAX) {
                qatomic_fetch_inc(&xwlc_overflow);
            } else {
                qatomic_set(&s->calls, calls + 1);
                if (busy) {
                    qatomic_fetch_inc(&s->busy);
                }
            }
            return;
        }
    }
    qatomic_fetch_inc(&xwlc_overflow);
}

/* Counts are cumulative, vCPU-only. 'lock' is the existing wrapper's initial
 * trylock, 'try' includes retry-loop attempts, 'bql' counts lock requests at
 * their actual caller, and 'bql-wait' counts contended BQL acquisitions.
 * Counts do not measure lock time or completed device/ISR work.
 */
EMSCRIPTEN_KEEPALIVE const char *xemu_wasm_lock_census_top(void)
{
    static char out[32768];
    static const char *names[] = {"lock", "try", "bql", "bql-wait"};
    size_t n = snprintf(out, sizeof(out), "overflow|%u\n",
                        qatomic_read(&xwlc_overflow));

    for (unsigned i = 0; i < XWLC_SLOTS; i++) {
        typeof(xwlc_sites[0]) *s = &xwlc_sites[i];
        const char *file = qatomic_load_acquire(&s->file);
        if (file) {
            const char *base = strrchr(file, '/');
            n += snprintf(out + n, sizeof(out) - n, "%s|%.160s:%d|%u|%u\n",
                          names[s->kind], base ? base + 1 : file, s->line,
                          qatomic_read(&s->calls), qatomic_read(&s->busy));
        }
    }
    return out;
}
