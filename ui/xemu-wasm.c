/*
 * xemu-wasm UI shim: headless entry point + low-memory tripwire.
 * Rebuilt from the last good object's DWARF + disassembly after an
 * accidental in-place clobber of this file. The cprobe CPU-dump block
 * (tick 400: cpu_get_phys_page_attrs_debug + cpu_physical_memory_read
 * on the main thread) is intentionally NOT restored: it is the suspected
 * trigger of the heap-0 clobber (debug walk racing the live vCPU).
 */
#include "qemu/osdep.h"
#include "hw/core/cpu.h"
#include "qemu/main-loop.h"
#include "qemu/timer.h"
#include "system/system.h"
#include "system/replay.h"
#include "system/runstate.h"
#include "qapi/error.h"
#include "qapi/qapi-commands-block.h"
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
    xemu_wasm_dump_ramblocks();
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

    fprintf(stderr, "[stack] ok %s: stack=%lu tid=%lu tls=%p pself=%p\n",
            where, (unsigned long)sz, (unsigned long)(uintptr_t)self,
            (void *)0, self);
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
    if (!seen && milestone_nseen < MILESTONE_MAX) {
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

/* gui timer */
static QEMUTimer *s_gui_timer;

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
        static int warned;
        if (con && !warned) {
            warned = 1;
            fprintf(stderr, "[wasm-gui] tick: con=%p graphic=%d\n",
                    con, qemu_console_is_graphic(con));
        }
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
        (void)gui_ms;
    }
    xemu_wasm_lowmem_check("gui tick post-getms");
    xemu_wasm_service_disc();
    xemu_wasm_lowmem_check("gui tick pre-arm");
    timer_mod(s_gui_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 16);
}

int main(int argc, char **argv)
{
    int i, status;
    const char *sk;

    fprintf(stderr, "xemu_version: %s\n", xemu_version);
    fprintf(stderr, "xemu_commit: %s\n", xemu_commit);
    fprintf(stderr, "xemu_date: %s\n", xemu_date);
    fwrite("xemu_wasm: booting headless (null renderer)\n", 44, 1, stderr);
    fwrite("[MAIN] entered main()\n", 22, 1, stderr);

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
        fprintf(stderr, "[MAIN] renderer=%s\n", rend ? rend : "null");
    }

    fwrite("[MAIN] calling qemu_init\n", 25, 1, stderr);
    sk = getenv("XEMU_WASM_SKIP");
    fprintf(stderr, "[MAIN] XEMU_WASM_SKIP=%s\n", sk ? sk : "(unset)");
    qemu_init(argc, argv);
    fwrite("[MAIN] qemu_init returned\n", 26, 1, stderr);

    fprintf(stderr, "[layout] stack %p-%p data_end %p heap_base %p global_base %p\n",
            (void *)emscripten_stack_get_end(), (void *)emscripten_stack_get_base(),
            (void *)&__data_end, (void *)&__heap_base, (void *)&__global_base);

    xemu_wasm_dump_ramblocks();

    /* re-establish the guard pattern after qemu_init allocations */
    lowmem_fill_pattern();
    lowmem_snapshot();
    lowmem_init = 1;

    s_gui_timer = timer_new(QEMU_CLOCK_VIRTUAL, SCALE_MS,
                            xemu_wasm_gui_tick, NULL);
    timer_mod(s_gui_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 16);
    fwrite("[wasm-gui] timer armed\n", 23, 1, stderr);

    bql_unlock();
    replay_mutex_unlock();
    replay_mutex_lock();
    bql_lock();
    fwrite("[MAIN] entering qemu_main_loop\n", 31, 1, stderr);
    status = qemu_main_loop();
    fprintf(stderr, "[MAIN] qemu_main_loop returned status=%d\n", status);
    qemu_cleanup(status);
    fwrite("[MAIN] qemu_cleanup done\n", 25, 1, stderr);
    bql_unlock();
    replay_mutex_unlock();
    return status;
}
