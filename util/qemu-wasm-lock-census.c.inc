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
static uint64_t xwlc_holder;
#define XWLC_WAIT_SLOTS 64
static struct {
    const char *file;
    int line;
    uint64_t holder, ns;
    uint32_t calls;
} xwlc_waits[XWLC_WAIT_SLOTS];
static uint32_t xwlc_wait_overflow;
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

/* Wasm32 file pointer + line in ONE atomic word. Every BQL owner publishes,
 * including condition-wait reacquisition. Clearing before release reports a
 * release window as unknown, rather than pretending the previous owner holds.
 */
void xemu_wasm_lock_census_publish(const char *file, int line)
{
    uint64_t site = file ? ((uint64_t)(uint32_t)(uintptr_t)file << 32) |
                           (uint32_t)line : 0;
    qatomic_set(&xwlc_holder, site);
}

uint64_t xemu_wasm_lock_census_holder(void)
{
    return qatomic_read(&xwlc_holder);
}

void xemu_wasm_lock_census_wait(const char *file, int line, uint64_t holder,
                               int64_t ns)
{
    unsigned hash = ((uintptr_t)file >> 3) ^ (line * 31u) ^ holder ^
                    (holder >> 32);

    for (unsigned i = 0; i < XWLC_WAIT_SLOTS; i++) {
        unsigned slot = (hash + i) & (XWLC_WAIT_SLOTS - 1);
        typeof(xwlc_waits[0]) *s = &xwlc_waits[slot];
        if (!s->file) {
            s->line = line;
            s->holder = holder;
            qatomic_store_release(&s->file, file);
        }
        if (s->file == file && s->line == line && s->holder == holder) {
            uint32_t calls = qatomic_read(&s->calls);
            if (calls == UINT32_MAX) {
                qatomic_fetch_inc(&xwlc_wait_overflow);
            } else {
                qatomic_set(&s->calls, calls + 1);
                qatomic_set(&s->ns, qatomic_read(&s->ns) +
                            (ns > 0 ? (uint64_t)ns : 0));
            }
            return;
        }
    }
    qatomic_fetch_inc(&xwlc_wait_overflow);
}

/* Counts are cumulative, vCPU-only. 'lock' is the existing wrapper's initial
 * trylock, 'try' includes retry-loop attempts, 'bql' counts lock requests at
 * their actual caller, and 'bql-wait' counts contended BQL acquisitions.
 * Counts do not measure lock time or completed device/ISR work.
 */
EMSCRIPTEN_KEEPALIVE const char *xemu_wasm_lock_census_top(void)
{
    static char out[65536];
    static const char *names[] = {"lock", "try", "bql", "bql-wait"};
    size_t n = snprintf(out, sizeof(out), "overflow|%u\nwait-overflow|%u\n",
                        qatomic_read(&xwlc_overflow),
                        qatomic_read(&xwlc_wait_overflow));

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
    /* Total WAIT time attributed to the holder observed near first failure.
     * Not a measurement of that holder's entire critical section: ownership
     * may change before acquisition. Unknown/release windows stay explicit.
     */
    for (unsigned i = 0; i < XWLC_WAIT_SLOTS; i++) {
        typeof(xwlc_waits[0]) *s = &xwlc_waits[i];
        const char *file = qatomic_load_acquire(&s->file);
        if (file) {
            const char *base = strrchr(file, '/');
            const char *hf = (const char *)(uintptr_t)(s->holder >> 32);
            const char *hb = hf ? strrchr(hf, '/') : NULL;
            n += snprintf(out + n, sizeof(out) - n,
                          "wait|%.160s:%d|%.160s:%u|%u|%" PRIu64 "\n",
                          base ? base + 1 : file, s->line,
                          hb ? hb + 1 : hf ? hf : "?",
                          (uint32_t)s->holder, qatomic_read(&s->calls),
                          qatomic_read(&s->ns));
        }
    }
    return out;
}
