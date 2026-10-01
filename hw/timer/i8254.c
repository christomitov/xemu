/*
 * QEMU 8253/8254 interval timer emulation
 *
 * Copyright (c) 2003-2004 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "qemu/xemu-wasm-stats.h"
#include "hw/irq.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/timer/i8254.h"
#include "hw/timer/i8254_internal.h"
#include "qom/object.h"
#include "trace.h"

//#define DEBUG_PIT

#define RW_STATE_LSB 1
#define RW_STATE_MSB 2
#define RW_STATE_WORD0 3
#define RW_STATE_WORD1 4

typedef struct PITClass PITClass;
DECLARE_CLASS_CHECKERS(PITClass, PIT,
                       TYPE_I8254)

struct PITClass {
    PITCommonClass parent_class;

    DeviceRealize parent_realize;
};

static void pit_irq_timer_update(PITChannelState *s, int64_t current_time);

static int pit_get_count(PITChannelState *s)
{
    uint64_t d;
    int counter;

    d = muldiv64(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - s->count_load_time, PIT_FREQ,
                 NANOSECONDS_PER_SECOND);
    switch(s->mode) {
    case 0:
    case 1:
    case 4:
    case 5:
        counter = (s->count - d) & 0xffff;
        break;
    case 3:
        /* XXX: may be incorrect for odd counts */
        counter = s->count - ((2 * d) % s->count);
        break;
    default:
        counter = s->count - (d % s->count);
        break;
    }
    return counter;
}

/* val must be 0 or 1 */
static void pit_set_channel_gate(PITCommonState *s, PITChannelState *sc,
                                 int val)
{
    switch (sc->mode) {
    default:
    case 0:
    case 4:
        /* XXX: just disable/enable counting */
        break;
    case 1:
    case 5:
        if (sc->gate < val) {
            /* restart counting on rising edge */
            sc->count_load_time = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
            pit_irq_timer_update(sc, sc->count_load_time);
        }
        break;
    case 2:
    case 3:
        if (sc->gate < val) {
            /* restart counting on rising edge */
            sc->count_load_time = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
            pit_irq_timer_update(sc, sc->count_load_time);
        }
        /* XXX: disable/enable counting */
        break;
    }
    sc->gate = val;
}

static inline void pit_load_count(PITChannelState *s, int val)
{
    if (val == 0)
        val = 0x10000;
    s->count_load_time = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->count = val;
#ifdef EMSCRIPTEN
    {
        extern void xemu_wasm_dbg_ring_put(const char *, ...);
        xemu_wasm_dbg_ring_put("[pit-load] count=%d mode=%d rw=%d tid=%lu\n",
                val, s->mode, s->rw_mode,
                (unsigned long)pthread_self());
    }
#endif
    pit_irq_timer_update(s, s->count_load_time);
}

/* if already latched, do not latch again */
static void pit_latch_count(PITChannelState *s)
{
    if (!s->count_latched) {
        s->latched_count = pit_get_count(s);
        s->count_latched = s->rw_mode;
    }
}

static void pit_ioport_write(void *opaque, hwaddr addr,
                             uint64_t val, unsigned size)
{
    PITCommonState *pit = opaque;
    int channel, access;
    PITChannelState *s;

    trace_pit_ioport_write(addr, val);

    addr &= 3;
    if (addr == 3) {
        channel = val >> 6;
        if (channel == 3) {
            /* read back command */
            for(channel = 0; channel < 3; channel++) {
                s = &pit->channels[channel];
                if (val & (2 << channel)) {
                    if (!(val & 0x20)) {
                        pit_latch_count(s);
                    }
                    if (!(val & 0x10) && !s->status_latched) {
                        /* status latch */
                        /* XXX: add BCD and null count */
                        s->status =
                            (pit_get_out(s,
                                         qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)) << 7) |
                            (s->rw_mode << 4) |
                            (s->mode << 1) |
                            s->bcd;
                        s->status_latched = 1;
                    }
                }
            }
        } else {
            s = &pit->channels[channel];
            access = (val >> 4) & 3;
            if (access == 0) {
                pit_latch_count(s);
            } else {
                s->rw_mode = access;
                s->read_state = access;
                s->write_state = access;

                s->mode = (val >> 1) & 7;
                s->bcd = val & 1;
                /* XXX: update irq timer ? */
            }
        }
    } else {
        s = &pit->channels[addr];
        switch(s->write_state) {
        default:
        case RW_STATE_LSB:
            pit_load_count(s, val);
            break;
        case RW_STATE_MSB:
            pit_load_count(s, val << 8);
            break;
        case RW_STATE_WORD0:
            s->write_latch = val;
            s->write_state = RW_STATE_WORD1;
            break;
        case RW_STATE_WORD1:
#ifdef EMSCRIPTEN
            {
                extern void xemu_wasm_dbg_ring_put(const char *, ...);
                xemu_wasm_dbg_ring_put("[pit-word] lsb=%d msb=%d count=%d\n",
                        (int)s->write_latch, (int)val,
                        (int)(s->write_latch | (val << 8)));
            }
#endif
            pit_load_count(s, s->write_latch | (val << 8));
            s->write_state = RW_STATE_WORD0;
            break;
        }
    }
}

static uint64_t pit_ioport_read(void *opaque, hwaddr addr,
                                unsigned size)
{
    PITCommonState *pit = opaque;
    int ret, count;
    PITChannelState *s;

    addr &= 3;

    if (addr == 3) {
        /* Mode/Command register is write only, read is ignored */
        return 0;
    }

    s = &pit->channels[addr];
    if (s->status_latched) {
        s->status_latched = 0;
        ret = s->status;
    } else if (s->count_latched) {
        switch(s->count_latched) {
        default:
        case RW_STATE_LSB:
            ret = s->latched_count & 0xff;
            s->count_latched = 0;
            break;
        case RW_STATE_MSB:
            ret = s->latched_count >> 8;
            s->count_latched = 0;
            break;
        case RW_STATE_WORD0:
            ret = s->latched_count & 0xff;
            s->count_latched = RW_STATE_MSB;
            break;
        }
    } else {
        switch(s->read_state) {
        default:
        case RW_STATE_LSB:
            count = pit_get_count(s);
            ret = count & 0xff;
            break;
        case RW_STATE_MSB:
            count = pit_get_count(s);
            ret = (count >> 8) & 0xff;
            break;
        case RW_STATE_WORD0:
            count = pit_get_count(s);
            ret = count & 0xff;
            s->read_state = RW_STATE_WORD1;
            break;
        case RW_STATE_WORD1:
            count = pit_get_count(s);
            ret = (count >> 8) & 0xff;
            s->read_state = RW_STATE_WORD0;
            break;
        }
    }

    trace_pit_ioport_read(addr, ret);

    return ret;
}

static void pit_irq_timer_update(PITChannelState *s, int64_t current_time)
{
    int64_t expire_time;
    int irq_level;

    if (!s->irq_timer || s->irq_disabled) {
        return;
    }
#ifdef EMSCRIPTEN
    extern void xemu_wasm_lowmem_check(const char *);
    xemu_wasm_lowmem_check("pit update pre");
    xemu_wasm_dbg_ring_put("[pit] s=%p irq_timer=%p irq=%p mode=%d\n", (void*)s, (void*)s->irq_timer, (void*)s->irq, s->mode);
#endif
    expire_time = pit_get_next_transition_time(s, current_time);
#ifdef EMSCRIPTEN
    {
        /* The virtual clock runs at real-time rate but each timer fire costs
         * ~1.4ms of real time on wasm. If the chain falls behind (e.g. after
         * reset, where load_time=0 seeds a 29ms period), each fire advances
         * the expire by only one period and can never catch up — the main
         * loop livelocks in the PIT cb. Coalesce: skip whole missed periods
         * so one fire brings the chain back into the future. */
        int64_t now_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        /*
         * Only when far behind: a rate-generator tick is two transitions
         * ~0.8 us apart (out low, then high = the IRQ edge), so the second
         * one is nearly always in the past when the callback runs. Skipping
         * there dropped about 2 of every 3 ticks (~350 timer IRQs/s instead
         * of 1000), and the guest's tick count, which games time animations
         * with, ran at a third of real speed. Short delays catch up in order.
         */
        if (expire_time != -1 && expire_time < now_ns - 100 * SCALE_MS) {
            int64_t ticks = (s->mode == 3) ? (s->count / 2) : s->count;
            if (ticks > 1) {
                int64_t period_ns = muldiv64(ticks, NANOSECONDS_PER_SECOND,
                                             PIT_FREQ);
                if (period_ns > 0) {
                    int64_t missed = (now_ns - expire_time) / period_ns + 1;
                    expire_time += missed * period_ns;
                }
            }
        }
    }
#endif
#ifdef EMSCRIPTEN
    {
        /* rate probe: count consecutive fires of this channel; every 256
         * fires print the virtual clock so the real cost per fire can be
         * computed from two consecutive prints. */
        static unsigned pit_fires;
        static int64_t last_report;
        extern void xemu_wasm_dbg_ring_put(const char *, ...);
        if (++pit_fires % 32 == 0) {
            int64_t nowv = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
            struct timespec rts;
            clock_gettime(CLOCK_REALTIME, &rts);
            int64_t rtms = rts.tv_sec * 1000LL + rts.tv_nsec / 1000000;
            (void)rtms;
            last_report = nowv;
        }
    }
#endif
#ifdef EMSCRIPTEN
    xemu_wasm_lowmem_check("pit after next_transition");
    {
        /* one-shot storm dump: if the PIT rearm is absurdly soon (period
         * under 2ms), the guest or a corrupt write programmed a tiny count
         * and the timer cb livelocks the main loop. Dump state once. */
        static int storm_dumped;
        if (!storm_dumped && expire_time - current_time < 2000000LL &&
            expire_time > current_time) {
            storm_dumped = 1;
            {
                uint64_t dd = muldiv64(current_time - s->count_load_time, PIT_FREQ,
                                       NANOSECONDS_PER_SECOND);
                if (0) fprintf(stderr, "[pit] STORM count=%d mode=%d gate=%d "
                        "load_time=%lld now=%lld expire=%lld delta=%lld "
                        "d=%llu base=%llu\n",
                        s->count, s->mode, s->gate,
                        (long long)s->count_load_time, (long long)current_time,
                        (long long)expire_time,
                        (long long)(expire_time - current_time),
                        (unsigned long long)dd,
                        (unsigned long long)QEMU_ALIGN_DOWN(dd, s->count));
            }
        }
    }
#endif
    irq_level = pit_get_out(s, current_time);
    if (irq_level && !s->wasm_last_out) {
        XSTAT_INC(n_pit_tick);
    }
    s->wasm_last_out = irq_level;
#ifdef EMSCRIPTEN
    xemu_wasm_lowmem_check("pit after get_out");
#endif
    qemu_set_irq(s->irq, irq_level);
#ifdef EMSCRIPTEN
    xemu_wasm_lowmem_check("pit after set_irq");
#endif
#ifdef DEBUG_PIT
    printf("irq_level=%d next_delay=%f\n",
           irq_level,
           (double)(expire_time - current_time) / NANOSECONDS_PER_SECOND);
#endif
    s->next_transition_time = expire_time;
    if (expire_time != -1)
        timer_mod(s->irq_timer, expire_time);
    else
        timer_del(s->irq_timer);
}

static void pit_irq_timer(void *opaque)
{
    PITChannelState *s = opaque;

    pit_irq_timer_update(s, s->next_transition_time);
}

static void pit_reset(DeviceState *dev)
{
    PITCommonState *pit = PIT_COMMON(dev);
    PITChannelState *s;

    pit_reset_common(pit);

    s = &pit->channels[0];
    if (!s->irq_disabled) {
        timer_mod(s->irq_timer, s->next_transition_time);
    }
}

/* When HPET is operating in legacy mode, suppress the ignored timer IRQ,
 * reenable it when legacy mode is left again. */
static void pit_irq_control(void *opaque, int n, int enable)
{
    PITCommonState *pit = opaque;
    PITChannelState *s = &pit->channels[0];

    if (enable) {
        s->irq_disabled = 0;
        pit_irq_timer_update(s, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    } else {
        s->irq_disabled = 1;
        timer_del(s->irq_timer);
    }
}

static const MemoryRegionOps pit_ioport_ops = {
    .read = pit_ioport_read,
    .write = pit_ioport_write,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void pit_post_load(PITCommonState *s)
{
    PITChannelState *sc = &s->channels[0];

    if (sc->next_transition_time != -1 && !sc->irq_disabled) {
        timer_mod(sc->irq_timer, sc->next_transition_time);
    } else {
        timer_del(sc->irq_timer);
    }
}

static void pit_realizefn(DeviceState *dev, Error **errp)
{
    PITCommonState *pit = PIT_COMMON(dev);
    PITClass *pc = PIT_GET_CLASS(dev);
    PITChannelState *s;

    s = &pit->channels[0];
    /* the timer 0 is connected to an IRQ */
    s->irq_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, pit_irq_timer, s);
    qdev_init_gpio_out(dev, &s->irq, 1);

    memory_region_init_io(&pit->ioports, OBJECT(pit), &pit_ioport_ops,
                          pit, "pit", 4);

    qdev_init_gpio_in(dev, pit_irq_control, 1);

    pc->parent_realize(dev, errp);
}

static void pit_class_initfn(ObjectClass *klass, const void *data)
{
    PITClass *pc = PIT_CLASS(klass);
    PITCommonClass *k = PIT_COMMON_CLASS(klass);
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_parent_realize(dc, pit_realizefn, &pc->parent_realize);
    k->set_channel_gate = pit_set_channel_gate;
    k->get_channel_info = pit_get_channel_info_common;
    k->post_load = pit_post_load;
    device_class_set_legacy_reset(dc, pit_reset);
}

static const TypeInfo pit_info = {
    .name          = TYPE_I8254,
    .parent        = TYPE_PIT_COMMON,
    .instance_size = sizeof(PITCommonState),
    .class_init    = pit_class_initfn,
    .class_size    = sizeof(PITClass),
};

static void pit_register_types(void)
{
    type_register_static(&pit_info);
}

type_init(pit_register_types)
