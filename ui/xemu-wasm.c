/*
 * xemu-wasm UI shim: headless entry point + low-memory tripwire.
 * Rebuilt from the last good object's DWARF + disassembly after an
 * accidental in-place clobber of this file. The cprobe CPU-dump block
 * (tick 400: cpu_get_phys_page_attrs_debug + cpu_physical_memory_read
 * on the main thread) is intentionally NOT restored: it is the suspected
 * trigger of the heap-0 clobber (debug walk racing the live vCPU).
 */
#include "qemu/osdep.h"
#include "qemu/xemu-wasm-stats.h"
#include "hw/core/cpu.h"
#include "qemu/main-loop.h"
#include "qemu/timer.h"
#include "system/system.h"
#include "system/replay.h"
#include "system/runstate.h"
#include "qapi/error.h"
#include "qapi/qapi-commands-block.h"
#include "qapi/qapi-commands-migration.h"
#include "migration/snapshot.h"
#include "qapi/qapi-commands-misc.h"
#include "hw/xbox/smbus.h"
#include "ui/xemu-settings.h"
#include <emscripten.h>
#include <emscripten/threading.h>
#include <emscripten/stack.h>

/* xemu externals (avoid pulling UI headers with epoxy/gl deps) */
extern const char *xemu_version;
extern const char *xemu_commit;
extern const char *xemu_date;
int __small_fprintf(void *stream, const char *fmt, ...);
void xemu_settings_set_path(const char *path);
struct QemuConsole;
typedef struct QemuConsole QemuConsole;
QemuConsole *qemu_console_lookup_by_index(unsigned int index);
int qemu_console_is_graphic(QemuConsole *con);
void graphic_hw_update(QemuConsole *con);
void nv2a_wasm_request_present(void);
extern char __data_end[];
extern char __heap_base[];
extern char __global_base[];

#define LOWMEM_SNAP_SIZE 65536u

/* snapshot of the guard zone (linear memory 0..64K) taken at init;
 * compared byte-for-byte on every check */
static uint8_t lowmem_snap[LOWMEM_SNAP_SIZE];
static uint32_t lowmem_init;

void xemu_wasm_dump_ramblocks(void);

/* ring buffer of instrumentation events (8K, keeps the most recent
 * entries, NUL-separated); dumped to stderr on tripwire events */
#define RING_SIZE 8192
static char ring[RING_SIZE];
static uint32_t ring_len;
static uint8_t ring_addr_printed;

static int64_t getutcms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + (int64_t)ts.tv_nsec / 1000000;
}

void xemu_wasm_cpu_dump(void)
{
    CPUState *cpu = qemu_get_cpu(0);
    if (cpu) {
        cpu_dump_state(cpu, stderr, 0x30000);
    }
}

uintptr_t xemu_wasm_stack_size(void)
{
    return (uintptr_t)emscripten_stack_get_base()
         - (uintptr_t)emscripten_stack_get_end();
}

void xemu_wasm_assert_stack(size_t min_bytes, const char *where)
{
    size_t sz = xemu_wasm_stack_size();
    void *self = pthread_self();

    if (sz < min_bytes) {
        fprintf(stderr, "[stack] FAIL %s: stack=%lu < required=%lu tid=%lu\n",
                where, (unsigned long)sz, (unsigned long)min_bytes,
                (unsigned long)(uintptr_t)self);
        abort();
    }
}

static void lowmem_fill_pattern(void)
{
    /* keep the two Emscripten cookies at 0-3 and 8-11; pattern the rest */
    memset((void *)12, 0xa5, LOWMEM_SNAP_SIZE - 12);
}

static void lowmem_snapshot(void)
{
    /* copy the guard zone byte-by-byte: a memcpy from linear-memory
     * offset 0 is literal-NULL UB to clang and gets optimized away,
     * truncating everything after it (this deleted the rest of main) */
    volatile const uint8_t *src = (volatile const uint8_t *)0;
    for (uint32_t k = 0; k < LOWMEM_SNAP_SIZE; k++) {
        lowmem_snap[k] = src[k];
    }
    lowmem_init = 1;
}

static void clobber_dump(int first)
{
    int from = first > 0 ? first : 0;
    int i, k;

    from = from < 65408 ? from : 65408;
    fprintf(stderr, "[tripwire] dump from %d:\n", from);
    fwrite("[tripwire] hex:", 15, 1, stderr);
    for (i = 0; i < 128; i++) {
        fprintf(stderr, " %02x", *(const uint8_t *)(from + i));
    }
    fputc('\n', stderr);
    fwrite("[tripwire] f32:", 15, 1, stderr);
    for (k = 0; k < 32; k++) {
        uint32_t u = (uint32_t)*(const uint8_t *)(from + 4 * k)
                   | (uint32_t)*(const uint8_t *)(from + 4 * k + 1) << 8
                   | (uint32_t)*(const uint8_t *)(from + 4 * k + 2) << 16
                   | (uint32_t)*(const uint8_t *)(from + 4 * k + 3) << 24;
        float v;
        memcpy(&v, &u, sizeof(v));
        __small_fprintf(stderr, " %g", (double)v);
    }
    fputc('\n', stderr);
    fwrite("[tripwire] i16:", 15, 1, stderr);
    for (i = 0; i < 64; i++) {
        int16_t v = (int16_t)((uint16_t)*(const uint8_t *)(from + 2 * i)
                    | (uint16_t)*(const uint8_t *)(from + 2 * i + 1) << 8);
        fprintf(stderr, " %d", v);
    }
    fputc('\n', stderr);
}

static void ring_vput(const char *fmt, va_list ap)
{
#ifndef XEMU_WASM_TRIPWIRE
    /* debug event ring: off by default, it sits on hot paths (main loop
     * poll, pfifo/apu loops, timers) and costs two printfs + a memmove */
    return;
#endif
    char tmp[256];
    int n, n2, n3;
    uint32_t len;

    memset(tmp, 0, sizeof(tmp));
    n = snprintf(tmp, sizeof(tmp), "[tid=%lu] ",
                 (unsigned long)(uintptr_t)pthread_self());
    if (n > 255) {
        return;
    }
    n2 = vsnprintf(tmp + n, sizeof(tmp) - n, fmt, ap);
    if (n2 < 1) {
        return;
    }
    n3 = n + n2 + 1;
    if (n3 > RING_SIZE) {
        return;
    }
    len = ring_len;
    if (len + n3 > RING_SIZE - 2) {
        uint32_t shift = n3 >= (int)len ? len : len - (n3 + 1);
        if (shift < len) {
            memmove(ring, ring + shift, len - shift);
            len -= shift;
        } else {
            len = 0;
        }
    }
    if (n2 > 0) {
        memcpy(ring + len, tmp, n3);
        ring_len = len + n3;
    }
}

void xemu_wasm_dbg_ring_put(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    ring_vput(fmt, ap);
    va_end(ap);
}

void xemu_wasm_dbg_ring_dump(void)
{
    fwrite("[ring] begin\n", 13, 1, stderr);
    if (ring_len) {
        fwrite(ring, 1, ring_len, stderr);
    }
    fputc('\n', stderr);
    fwrite("[ring] end\n", 11, 1, stderr);
}

void xemu_wasm_dbg_ring_snapshot(const char *path)
{
#ifndef XEMU_WASM_TRIPWIRE
    /* was called on every poll enter/exit: each fopen/fwrite/fclose is a
     * syscall proxied to the browser main thread */
    return;
#endif
    if (!ring_addr_printed) {
        ring_addr_printed = 1;
        fprintf(stderr, "[ml] ring addr=%p len=%u\n", ring, ring_len);
    }
    FILE *f = fopen(path, "w");
    if (f) {
        fwrite(ring, 1, ring_len, f);
        fputc('\n', f);
        fclose(f);
    }
}

static uint32_t tripwire_seq;

unsigned long xemu_wasm_tripwire_seq(void)
{
    return tripwire_seq;
}

#define MILESTONE_MAX 16
static const char *milestone_seen[MILESTONE_MAX];
static int milestone_nseen;
static int64_t milestone_t0;

void xemu_wasm_milestone(const char *name)
{
    int i, seen = 0;

    for (i = 0; i < milestone_nseen; i++) {
        if (strcmp(milestone_seen[i], name) == 0) {
            seen = 1;
            break;
        }
    }
    if (seen) {
        return; /* log each milestone once */
    }
    if (milestone_nseen < MILESTONE_MAX) {
        milestone_seen[milestone_nseen++] = name;
    }
    if (milestone_t0 == 0) {
        milestone_t0 = getutcms();
    }
    fprintf(stderr, "[milestone] %s seq=%lu t=%lldms\n", name,
            (unsigned long)tripwire_seq, (long long)(getutcms() - milestone_t0));
}

static uint8_t soft;
static uint32_t soft_seen;
static const char *last_ok_check;
static const char *prev_ok_check;

void xemu_wasm_lowmem_check(const char *where)
{
    const uint8_t *cur = (const uint8_t *)0;
    int corrupt = 0;
    uint32_t i;

#ifndef XEMU_WASM_TRIPWIRE
    /* heap-0 clobber tripwire: off by default (the DSP DMA bug it hunted
     * is fixed); build with -DXEMU_WASM_TRIPWIRE to re-arm */
    return;
#endif
    if (!lowmem_init) {
        return;
    }
    prev_ok_check = last_ok_check;
    last_ok_check = where;
    tripwire_seq++;

    for (i = 0; i < LOWMEM_SNAP_SIZE; i++) {
        if (lowmem_snap[i] != *(const uint8_t *)i) {
            corrupt = 1;
            break;
        }
    }
    if (!corrupt) {
        return;
    }

    soft_seen++;
    if (!__atomic_exchange_n(&soft, 1, __ATOMIC_SEQ_CST)) {
        fprintf(stderr, "[tripwire] SOFT clobber detected at %s: seq=%lu tid=%lu\n",
                where, (unsigned long)tripwire_seq,
                (unsigned long)(uintptr_t)pthread_self());
        fprintf(stderr, "[tripwire] last-ok check: %s (prev: %s)\n",
                last_ok_check ? last_ok_check : "(none)",
                prev_ok_check ? prev_ok_check : "(none)");
        clobber_dump(0);
        xemu_wasm_dbg_ring_dump();
    }
    {
        /* plain volatile read: an atomic load from the literal null
         * pointer is UB and clang turns it into an unreachable trap */
        int cs = *(volatile int *)0;
        if (cs == 0) {
            fprintf(stderr, "[tripwire] vcpu halted=%d\n", cs);
            xemu_wasm_cpu_dump();
        }
    }
    fprintf(stderr, "[tripwire] corrupt@%s tid=%lu n=%d\n", where,
            (unsigned long)(uintptr_t)pthread_self(), soft_seen);
    if (soft_seen >= 300) {
        uint32_t j, count = 0;
        int32_t last = -1, first = -1;

        fprintf(stderr, "[tripwire] soft mode done, aborting: seen=%d\n",
                soft_seen);
        for (i = 0; i < LOWMEM_SNAP_SIZE; i++) {
            if (lowmem_snap[i] != *(const uint8_t *)i) {
                if (first < 0) {
                    first = i;
                }
                last = i;
                count++;
            }
        }
        fprintf(stderr, "[tripwire] clobber after %s: seq=%lu tid=%lu first=%d last=%d count=%d\n",
                where, (unsigned long)tripwire_seq,
                (unsigned long)(uintptr_t)pthread_self(), first, last, count);
        fprintf(stderr, "[tripwire] last-ok check: %s (prev: %s)\n",
                last_ok_check ? last_ok_check : "(none)",
                prev_ok_check ? prev_ok_check : "(none)");
        clobber_dump(first);
        xemu_wasm_dbg_ring_dump();
        if (count <= 65535) {
            for (j = 0; j < LOWMEM_SNAP_SIZE; j++) {
                if (lowmem_snap[j] != *(const uint8_t *)j) {
                    fprintf(stderr, "[tripwire]   @%d: %02x -> %02x\n", j,
                            lowmem_snap[j], *(const volatile uint8_t *)j);
                }
            }
        }
        abort();
    }
}

void xemu_queue_error_message(const char *msg)
{
    fprintf(stderr, "XEMU-ERROR: %s\n", msg);
}

void xemu_queue_notification(const char *msg)
{
    fprintf(stderr, "XEMU-NOTIFY: %s\n", msg);
}

const char *xemu_settings_get_base_path(void)
{
    return "/xemu";
}

const char *xemu_settings_get_default_eeprom_path(void)
{
    return "/xemu/eeprom.bin";
}

/*
 * Player 1 controller fed from the page (Gamepad API / keyboard+mouse).
 * The browser thread writes pad_in via xemu_wasm_set_pad(); the XID device
 * copies it into the bound ControllerState when the guest polls.
 */
#include "ui/xemu-input.h"
static ControllerState wasm_pad = { .name = "Browser", .bound = 0 };
static volatile uint32_t pad_buttons;
static volatile int16_t pad_axis[CONTROLLER_AXIS__COUNT];

EMSCRIPTEN_KEEPALIVE void xemu_wasm_set_pad(int buttons, int lt, int rt,
                                            int lx, int ly, int rx, int ry)
{
    pad_axis[CONTROLLER_AXIS_LTRIG] = lt;
    pad_axis[CONTROLLER_AXIS_RTRIG] = rt;
    pad_axis[CONTROLLER_AXIS_LSTICK_X] = lx;
    pad_axis[CONTROLLER_AXIS_LSTICK_Y] = ly;
    pad_axis[CONTROLLER_AXIS_RSTICK_X] = rx;
    pad_axis[CONTROLLER_AXIS_RSTICK_Y] = ry;
    __atomic_store_n(&pad_buttons, (uint32_t)buttons, __ATOMIC_RELEASE);
}

void xemu_input_update_controller(ControllerState *state)
{
    state->buttons = __atomic_load_n(&pad_buttons, __ATOMIC_ACQUIRE);
    for (int i = 0; i < CONTROLLER_AXIS__COUNT; i++) {
        state->axis[i] = pad_axis[i];
    }
}

void xemu_input_update_rumble(ControllerState *state)
{
    (void)state; /* TODO: forward to Gamepad.vibrationActuator */
}

ControllerState *xemu_input_get_bound(int index)
{
    return index == 0 ? &wasm_pad : NULL;
}

int xemu_input_get_test_mode(void)
{
    return 0;
}

int xemu_snapshots_save_extra_data(FILE *f)
{
    (void)f;
    return 0;
}

int xemu_snapshots_offset_extra_data(FILE *f)
{
    (void)f;
    return 0;
}

void xemu_snapshots_mark_dirty(void) {}

/*
 * Disc hot-load from the page. The browser thread only flips a flag (it must
 * not touch QEMU state); the gui tick picks it up on the main-loop thread
 * with the BQL held, swaps the DVD medium and optionally resets so the
 * console boots the disc.
 */
static volatile int disc_req; /* 0 none, 1 load, 2 load+reset, 3 eject */

/*
 * Machine state snapshots for the headless benchmark: the page (or the
 * bench) asks for one, the gui tick starts a migration to a file in MEMFS
 * and resumes the VM when it completes. A run started with
 * XEMU_WASM_INCOMING=<path> restores it (-incoming file:<path>).
 */
static volatile int state_req;
static volatile int state_status;   /* 0 idle, 1 saving, 2 saved, -1 failed */

static void xemu_wasm_service_state(void);
static QEMUTimer *s_state_timer;

/*
 * Realtime, not virtual: the virtual clock (and the gui tick on it) stops
 * while the VM is paused for the save, which is when completion must be
 * noticed and the VM resumed.
 */
static void state_timer_cb(void *opaque)
{
    xemu_wasm_service_state();
    timer_mod(s_state_timer, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + 100);
}

EMSCRIPTEN_KEEPALIVE void xemu_wasm_save_state(void)
{
    __atomic_store_n(&state_status, 1, __ATOMIC_SEQ_CST);
    __atomic_store_n(&state_req, 1, __ATOMIC_SEQ_CST);
}

EMSCRIPTEN_KEEPALIVE int xemu_wasm_save_state_status(void)
{
    return __atomic_load_n(&state_status, __ATOMIC_SEQ_CST);
}

static void xemu_wasm_service_state(void)
{
    Error *err = NULL;

    if (!__atomic_exchange_n(&state_req, 0, __ATOMIC_SEQ_CST)) {
        return;
    }
    /*
     * xemu's own snapshot path (main loop, BQL held): device save hooks
     * that need their worker threads idle cannot deadlock against a
     * migration thread holding the BQL. The snapshot is stored inside the
     * HDD image (/xemu/xbox_hdd.qcow2) as "bench"; restore with
     * XEMU_WASM_LOADVM=bench.
     */
    if (save_snapshot("bench", true, NULL, false, NULL, &err)) {
        fprintf(stderr, "[state] saved snapshot 'bench' in the HDD image\n");
        __atomic_store_n(&state_status, 2, __ATOMIC_SEQ_CST);
    } else {
        fprintf(stderr, "XEMU-ERROR: save state: %s\n",
                err ? error_get_pretty(err) : "failed");
        error_free(err);
        __atomic_store_n(&state_status, -1, __ATOMIC_SEQ_CST);
    }
}

EMSCRIPTEN_KEEPALIVE void xemu_wasm_request_disc(int mode)
{
    __atomic_store_n(&disc_req, mode, __ATOMIC_SEQ_CST);
}

static void xemu_wasm_service_disc(void)
{
    int req = __atomic_exchange_n(&disc_req, 0, __ATOMIC_SEQ_CST);
    Error *err = NULL;

    if (!req) {
        return;
    }
    xbox_smc_eject_button();
    if (req == 3) {
        qmp_eject("ide0-cd1", NULL, true, false, &err);
    } else {
        qmp_blockdev_change_medium("ide0-cd1", NULL, "/xemu/iso.iso", "raw",
                                   false, false, false, 0, &err);
    }
    if (err) {
        fprintf(stderr, "XEMU-ERROR: disc %s failed: %s\n",
                req == 3 ? "eject" : "load", error_get_pretty(err));
        error_free(err);
    } else {
        fprintf(stderr, "[disc] %s\n", req == 3 ? "ejected" : "loaded /xemu/iso.iso");
    }
    xbox_smc_update_tray_state();
    if (req == 2 && !err) {
        qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
    }
}

/* Performance counters for the page's FPS overlay (read from the browser
 * thread; plain aligned 32-bit words, torn reads don't matter here). */
volatile uint32_t xemu_wasm_present_count; /* frames shown on the canvas */
volatile uint32_t xemu_wasm_flip_count;    /* guest buffer flips */
static volatile uint32_t virt_ms_now;      /* emulated clock, ms */

EMSCRIPTEN_KEEPALIVE uint32_t xemu_wasm_get_present_count(void)
{
    return xemu_wasm_present_count;
}

EMSCRIPTEN_KEEPALIVE uint32_t xemu_wasm_get_flip_count(void)
{
    return xemu_wasm_flip_count;
}

EMSCRIPTEN_KEEPALIVE uint32_t xemu_wasm_get_virt_ms(void)
{
    return virt_ms_now;
}

XemuWasmStats xemu_wasm_stats;

int64_t xemu_wasm_stats_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/*
 * MMIO profile per MemoryRegion (keyed by name pointer; written on the vCPU
 * thread only). Reported by the page as mmio_top: calls and time per device.
 */
#define MMIO_PROF_SLOTS 128
static struct {
    const char *name;
    uint64_t calls, ns;
} mmio_prof[MMIO_PROF_SLOTS];

void xemu_wasm_mmio_prof(const char *region, int64_t ns)
{
    uintptr_t h = ((uintptr_t)region >> 3) * 2654435761u;
    for (unsigned i = 0; i < MMIO_PROF_SLOTS; i++) {
        unsigned slot = (h + i) & (MMIO_PROF_SLOTS - 1);
        if (mmio_prof[slot].name == region || !mmio_prof[slot].name) {
            mmio_prof[slot].name = region;
            mmio_prof[slot].calls++;
            mmio_prof[slot].ns += ns;
            return;
        }
    }
}

/* "name=calls/ms;..." for the 12 regions with the most time */
EMSCRIPTEN_KEEPALIVE const char *xemu_wasm_mmio_top(void)
{
    static char buf[1024];
    bool used[MMIO_PROF_SLOTS] = { false };
    int len = 0;

    buf[0] = 0;
    for (int k = 0; k < 12; k++) {
        int best = -1;
        for (int i = 0; i < MMIO_PROF_SLOTS; i++) {
            if (!used[i] && mmio_prof[i].name &&
                (best < 0 || mmio_prof[i].ns > mmio_prof[best].ns)) {
                best = i;
            }
        }
        if (best < 0 || len > (int)sizeof(buf) - 80) {
            break;
        }
        used[best] = true;
        len += snprintf(buf + len, sizeof(buf) - len, "%s=%llu/%llu;",
                        mmio_prof[best].name ? mmio_prof[best].name : "?",
                        (unsigned long long)mmio_prof[best].calls,
                        (unsigned long long)(mmio_prof[best].ns / 1000000));
    }
    return buf;
}

/*
 * Sampling profile (see xemu-wasm-stats.h): the main loop calls
 * xemu_wasm_phase_sample() every iteration; xemu_wasm_phase_top() returns
 * and resets the counts since its last call.
 */
const char *volatile xemu_wasm_phase[2];

#define PHASE_SLOTS 128
static struct {
    const char *name;
    uint32_t n;
} phase_prof[2][PHASE_SLOTS];

static void phase_bump(int t, const char *name)
{
    uintptr_t h = ((uintptr_t)name >> 2) * 2654435761u;
    for (unsigned i = 0; i < PHASE_SLOTS; i++) {
        unsigned slot = (h + i) & (PHASE_SLOTS - 1);
        if (phase_prof[t][slot].name == name || !phase_prof[t][slot].name) {
            phase_prof[t][slot].name = name;
            phase_prof[t][slot].n++;
            return;
        }
    }
}

/*
 * Sampled from its own thread at ~1 kHz: sampling from the main loop would
 * only ever look while main holds the BQL (and over-count "bql_wait").
 */
static void *phase_sampler_thread(void *opaque)
{
    for (;;) {
        usleep(1000);
        xemu_wasm_phase_sample();
    }
    return NULL;
}

void xemu_wasm_phase_sample(void)
{
    static const char *const dflt[2] = { "jit", "pfifo" };
    for (int t = 0; t < 2; t++) {
        const char *p = xemu_wasm_phase[t];
        phase_bump(t, p ? p : dflt[t]);
    }
}

/* "vcpu|name=n;name=n;...\ngpu|..." busiest first, counts since last call */
EMSCRIPTEN_KEEPALIVE const char *xemu_wasm_phase_top(void)
{
    static char buf[2048];
    static const char *const label[2] = { "vcpu", "gpu" };
    int len = 0;

    for (int t = 0; t < 2; t++) {
        bool used[PHASE_SLOTS] = { false };
        len += snprintf(buf + len, sizeof(buf) - len, "%s|", label[t]);
        for (int k = 0; k < 24; k++) {
            int best = -1;
            for (int i = 0; i < PHASE_SLOTS; i++) {
                if (!used[i] && phase_prof[t][i].n &&
                    (best < 0 || phase_prof[t][i].n > phase_prof[t][best].n)) {
                    best = i;
                }
            }
            if (best < 0 || len > (int)sizeof(buf) - 100) {
                break;
            }
            used[best] = true;
            len += snprintf(buf + len, sizeof(buf) - len, "%s=%u;",
                            phase_prof[t][best].name, phase_prof[t][best].n);
        }
        len += snprintf(buf + len, sizeof(buf) - len, "\n");
        for (int i = 0; i < PHASE_SLOTS; i++) {
            phase_prof[t][i].n = 0;
        }
    }
    return buf;
}

/* canvas size in physical pixels, from the page (display.c) */
EMSCRIPTEN_KEEPALIVE void xemu_wasm_set_display_size(int w, int h)
{
    extern volatile int xemu_wasm_display_w, xemu_wasm_display_h;
    xemu_wasm_display_w = w;
    xemu_wasm_display_h = h;
}

/*
 * Keyed event counts (interned strings), e.g. why surfaces get downloaded.
 * Single writer (pfifo thread) in practice; reported as downloads_top.
 */
#define COUNT_SLOTS 256
static struct {
    const char *key;
    uint32_t n;
} count_tab[COUNT_SLOTS];

void xemu_wasm_count(const char *key)
{
    uintptr_t h = ((uintptr_t)key >> 3) * 2654435761u;
    for (unsigned i = 0; i < COUNT_SLOTS; i++) {
        unsigned slot = (h + i) & (COUNT_SLOTS - 1);
        if (count_tab[slot].key == key || !count_tab[slot].key) {
            count_tab[slot].key = key;
            count_tab[slot].n++;
            return;
        }
    }
}

/* "key=n;..." top 60, cumulative */
EMSCRIPTEN_KEEPALIVE const char *xemu_wasm_count_top(void)
{
    static char buf[8192];
    bool used[COUNT_SLOTS] = { false };
    int len = 0;

    buf[0] = 0;
    for (int k = 0; k < 60; k++) {
        int best = -1;
        for (int i = 0; i < COUNT_SLOTS; i++) {
            if (!used[i] && count_tab[i].key &&
                (best < 0 || count_tab[i].n > count_tab[best].n)) {
                best = i;
            }
        }
        if (best < 0 || len > (int)sizeof(buf) - 200) {
            break;
        }
        used[best] = true;
        len += snprintf(buf + len, sizeof(buf) - len, "%s=%u;",
                        count_tab[best].key, count_tab[best].n);
    }
    return buf;
}

/* page reads the struct directly from the wasm heap */
EMSCRIPTEN_KEEPALIVE XemuWasmStats *xemu_wasm_stats_ptr(void)
{
    return &xemu_wasm_stats;
}

EMSCRIPTEN_KEEPALIVE const char *xemu_wasm_stats_fields(void)
{
#define XEMU_WASM_STATS_NAME(name) #name ","
    return XEMU_WASM_STATS_FIELDS(XEMU_WASM_STATS_NAME);
#undef XEMU_WASM_STATS_NAME
}

/* gui timer */
static QEMUTimer *s_gui_timer;

/*
 * The GUI tick raises the guest VBLANK: keep it at NTSC 59.94 Hz on an
 * absolute schedule. "now + 16 ms" gave 62.5 Hz plus the callback's
 * latency, so vsync-paced games ran ~4% fast and jittered.
 * XEMU_WASM_VBLANK_HZ overrides (e.g. 50 for PAL).
 */
static void gui_timer_arm(void)
{
    static int64_t next_ns, period_ns;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (!period_ns) {
        const char *e = getenv("XEMU_WASM_VBLANK_HZ");
        double hz = e ? atof(e) : 60000.0 / 1001.0;
        period_ns = (int64_t)(1e9 / (hz > 1 ? hz : 60000.0 / 1001.0));
    }
    next_ns += period_ns;
    if (next_ns < now - 100 * SCALE_MS || next_ns > now + 2 * period_ns) {
        next_ns = now + period_ns;      /* far behind (or first): resync */
    }
    timer_mod(s_gui_timer, next_ns);
}

static void xemu_wasm_gui_tick(void *opaque)
{
    static int tickn;
    (void)opaque;

    xemu_wasm_lowmem_check("gui tick");

    tickn++;
    if (tickn % 625 == 0) {
        int64_t virt, rt, host;
        xemu_wasm_lowmem_check("gui tick pre-hb");
        virt = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        rt = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
        host = qemu_clock_get_ns(QEMU_CLOCK_HOST);
        fprintf(stderr, "[heartbeat] seq=%lu t=%dms virt=%lld rt=%lld host=%lld voff=%lld en=%d\n",
                (unsigned long)tripwire_seq, tickn * 16,
                (long long)virt, (long long)rt, (long long)host, 0LL, 0);
        xemu_wasm_lowmem_check("gui tick post-hb");
    }

    /* NOTE: the old cprobe==400 diagnostic block (cpu_get_phys_page_attrs_debug
     * + cpu_physical_memory_read from the GUI thread) is deliberately absent:
     * it raced the live vCPU's MMU and is the prime suspect for the heap-0
     * clobber this tripwire exists to catch. */

    xemu_wasm_lowmem_check("gui tick pre-mod");
    xemu_wasm_lowmem_check("gui tick pre-lookup");
    {
        QemuConsole *con = qemu_console_lookup_by_index(0);
        /* No display listener exists under -display none, so nothing else
         * refreshes the console: drive nv2a's gfx_update (which raises the
         * guest VBLANK interrupt and exports the frame to the page) here,
         * at the same ~60 Hz a native display would. */
        if (con) {
            graphic_hw_update(con);
        }
        nv2a_wasm_request_present();
    }
    xemu_wasm_lowmem_check("gui tick post-lookup");
    xemu_wasm_lowmem_check("gui tick pre-getms");
    {
        int64_t gui_ms = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
        virt_ms_now = (uint32_t)gui_ms;
    }
    xemu_wasm_lowmem_check("gui tick post-getms");
    xemu_wasm_service_disc();
    xemu_wasm_lowmem_check("gui tick pre-arm");
    gui_timer_arm();
}

int main(int argc, char **argv)
{
    int i, status;
    const char *sk;

    fprintf(stderr, "xemu_version: %s\n", xemu_version);
    fprintf(stderr, "xemu_commit: %s\n", xemu_commit);
    fprintf(stderr, "xemu_date: %s\n", xemu_date);

    xemu_wasm_assert_stack(0xF00000, "main");
    lowmem_fill_pattern();
    lowmem_snapshot();

    for (i = 1; i < argc; i++) {
        if (argv[i] && strcmp(argv[i], "-config_path") == 0) {
            argv[i] = NULL;
            if (i >= argc - 1) {
                break;
            }
            if (argv[i + 1]) {
                xemu_settings_set_path(argv[i + 1]);
            }
            argv[i + 1] = NULL;
            break;
        }
    }

    /* The page sets XEMU_RENDERER=webgpu when navigator.gpu exists; the
     * settings constructor ran before the page could set ENV, so apply the
     * renderer choice here, before qemu_init creates the NV2A. */
    {
        const char *rend = getenv("XEMU_RENDERER");
        if (rend && strcmp(rend, "webgpu") == 0) {
            g_config.display.renderer = CONFIG_DISPLAY_RENDERER_WEBGPU;
        }
        /* full startup animation unless the page asks to skip it
         * (XEMU_BOOT_ANIM=0, page URL ?noanim) */
        const char *anim = getenv("XEMU_BOOT_ANIM");
        g_config.general.skip_boot_anim = anim && strcmp(anim, "0") == 0;
    }

    fwrite("[MAIN] calling qemu_init\n", 25, 1, stderr);
    sk = getenv("XEMU_WASM_SKIP");
    {
        /* XEMU_WASM_INCOMING=<path>: restore a saved machine state */
        /* XEMU_WASM_DVD=<path>: boot with that disc in the drive */
        const char *in = getenv("XEMU_WASM_INCOMING");
        const char *dvd = getenv("XEMU_WASM_DVD");
        char **nargv = g_new0(char *, argc + 7);
        for (i = 0; i < argc; i++) {
            nargv[i] = argv[i];
        }
        if (in && *in) {
            nargv[argc++] = (char *)"-incoming";
            nargv[argc++] = g_strdup_printf("file:%s", in);
        }
        /* XEMU_WASM_LOADVM=<name>: start from a snapshot in the HDD image */
        const char *lvm = getenv("XEMU_WASM_LOADVM");
        if (lvm && *lvm) {
            nargv[argc++] = (char *)"-loadvm";
            nargv[argc++] = (char *)lvm;
        }
        if (dvd && *dvd) {
            nargv[argc++] = (char *)"-dvd_path";
            nargv[argc++] = (char *)dvd;
        }
        argv = nargv;
    }
    qemu_init(argc, argv);
    fwrite("[MAIN] qemu_init returned\n", 26, 1, stderr);


    xemu_wasm_dump_ramblocks();

    /* re-establish the guard pattern after qemu_init allocations */
    lowmem_fill_pattern();
    lowmem_snapshot();
    lowmem_init = 1;

    {
        static QemuThread sampler;
        qemu_thread_create(&sampler, "phase-sampler", phase_sampler_thread,
                           NULL, QEMU_THREAD_DETACHED);
    }
    s_gui_timer = timer_new(QEMU_CLOCK_VIRTUAL, SCALE_NS,
                            xemu_wasm_gui_tick, NULL);
    s_state_timer = timer_new_ms(QEMU_CLOCK_REALTIME, state_timer_cb, NULL);
    timer_mod(s_state_timer, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + 100);
    gui_timer_arm();

    bql_unlock();
    replay_mutex_unlock();
    replay_mutex_lock();
    bql_lock();
    fwrite("[MAIN] entering qemu_main_loop\n", 31, 1, stderr);
    status = qemu_main_loop();
    qemu_cleanup(status);
    fwrite("[MAIN] qemu_cleanup done\n", 25, 1, stderr);
    bql_unlock();
    replay_mutex_unlock();
    return status;
}
