/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Included by the Wasm lock census. One writer: the main-loop thread.
 * Exclusive elapsed time while holding BQL, not wall time across cond waits.
 * Scope tokens live on the original C/Asyncify stack; no new continuation.
 */
#define XWLD_SLOTS 128
static const char *const xwld_names[] = {
    "ml:outer", "ml:setup", "ml:glib-prepare", "ml:glib-check",
    "ml:glib-dispatch", "ml:notify", "ml:deadline", "ml:timers",
    "ml:timer", "ml:bh", "ml:fd-read", "ml:fd-write", "ml:fd-poll",
};
static struct {
    uint64_t tag, ns;
    uint32_t calls;
} xwld_sites[XWLD_SLOTS];
static uint32_t xwld_overflow;
static __thread bool xwld_main_thread, xwld_held;
static __thread unsigned xwld_current;
static __thread int64_t xwld_last;
bool xemu_wasm_bql_detail_enabled;

static unsigned xwld_slot(unsigned kind, uint32_t id)
{
    uint64_t tag = ((uint64_t)(kind + 1) << 32) | id;
    unsigned hash = kind * 31u + id;

    assert(kind < G_N_ELEMENTS(xwld_names));
    for (unsigned i = 0; i < XWLD_SLOTS; i++) {
        unsigned slot = (hash + i) & (XWLD_SLOTS - 1);
        if (!xwld_sites[slot].tag) {
            qatomic_store_release(&xwld_sites[slot].tag, tag);
        }
        if (xwld_sites[slot].tag == tag) {
            return slot;
        }
    }
    qatomic_fetch_inc(&xwld_overflow);
    return XWLD_SLOTS;
}

static void xwld_flush(int64_t now)
{
    if (xwld_held && now > xwld_last) {
        uint64_t ns = qatomic_read(&xwld_sites[xwld_current].ns);
        qatomic_set(&xwld_sites[xwld_current].ns, ns + now - xwld_last);
    }
    xwld_last = now;
}

static uint64_t xwld_activity(void)
{
    uint64_t tag = xwld_sites[xwld_current].tag;
    unsigned kind = (tag >> 32) - 1;
    return ((uint64_t)(uint32_t)(uintptr_t)xwld_names[kind] << 32) |
           (uint32_t)tag;
}

/* Invoked only by the existing acquired/releasing BQL publications. Repeated
 * true publications (generic wrapper -> actual caller) are not transitions.
 */
static uint64_t xwld_owner(bool held, uint64_t site)
{
    if (qatomic_read(&xemu_wasm_bql_detail_enabled) && xwld_main_thread) {
        if (held != xwld_held) {
            xwld_flush(xemu_wasm_stats_now_ns());
            xwld_held = held;
        }
        if (held) {
            return xwld_activity();
        }
    }
    return site;
}

unsigned xemu_wasm_bql_detail_begin(unsigned kind, uint32_t id)
{
    unsigned slot, token;
    if (!xwld_main_thread) {
        return 0;
    }
    slot = xwld_slot(kind, id);
    if (slot == XWLD_SLOTS) {
        return 0;
    }
    xwld_flush(xemu_wasm_stats_now_ns());
    token = xwld_current + 1;
    xwld_current = slot;
    if (qatomic_read(&xwld_sites[slot].calls) == UINT32_MAX) {
        qatomic_fetch_inc(&xwld_overflow);
    } else {
        qatomic_fetch_inc(&xwld_sites[slot].calls);
    }
    if (xwld_held) {
        qatomic_set(&xwlc_holder, xwld_activity());
    }
    return token;
}

void xemu_wasm_bql_detail_end(unsigned token)
{
    if (token) {
        assert(token <= XWLD_SLOTS);
        xwld_flush(xemu_wasm_stats_now_ns());
        xwld_current = token - 1;
        if (xwld_held) {
            qatomic_set(&xwlc_holder, xwld_activity());
        }
    }
}

unsigned xemu_wasm_bql_detail_main(void)
{
    if (!xwld_main_thread) {
        xwld_current = xwld_slot(XWLD_OUTER, 0);
        assert(xwld_current < XWLD_SLOTS);
        xwld_held = bql_locked();
        xwld_last = xemu_wasm_stats_now_ns();
        xwld_main_thread = true;
    }
    return xemu_wasm_bql_detail_begin(XWLD_SETUP, 0);
}

static size_t xwld_format(char *out, size_t size)
{
    size_t n = snprintf(out, size, "detail-overflow|%u\n",
                        qatomic_read(&xwld_overflow));
    for (unsigned i = 0; i < XWLD_SLOTS; i++) {
        uint64_t tag = qatomic_load_acquire(&xwld_sites[i].tag);
        if (tag) {
            unsigned kind = (tag >> 32) - 1;
            n += snprintf(out + n, size - n, "detail|%s:%u|%u|%" PRIu64 "\n",
                          xwld_names[kind], (uint32_t)tag,
                          qatomic_read(&xwld_sites[i].calls),
                          qatomic_read(&xwld_sites[i].ns));
        }
    }
    return n;
}
