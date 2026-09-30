/*
 * MCPX APU audio output for the browser build.
 *
 * The native build plays each mixed APU frame through an SDL audio stream
 * (monitor.c). Here the frames go into a single-producer/single-consumer
 * ring buffer in wasm memory; the host page's AudioWorklet reads it from
 * the shared heap on the browser audio thread (see www/index.html).
 * 48 kHz, interleaved stereo int16. The producer never blocks: the
 * worklet skips ahead on overrun (and to bound latency) and plays silence
 * on underflow.
 */
#include "apu_int.h"
#include <emscripten.h>

#define WASM_AUDIO_RING_FRAMES 8192 /* ~170 ms at 48 kHz, power of two */

typedef struct WasmAudioRing {
    uint32_t write;    /* frames written (producer: APU thread) */
    uint32_t read;     /* frames read (consumer: AudioWorklet) */
    uint32_t capacity; /* frames, power of two */
    uint32_t rate;
    int16_t data[WASM_AUDIO_RING_FRAMES * 2];
} WasmAudioRing;

static WasmAudioRing wasm_audio_ring = {
    .capacity = WASM_AUDIO_RING_FRAMES,
    .rate = 48000,
};

EMSCRIPTEN_KEEPALIVE WasmAudioRing *xemu_wasm_audio_ring(void)
{
    return &wasm_audio_ring;
}

void mcpx_apu_monitor_init(MCPXAPUState *d, Error **errp)
{
    /* no SDL stream: the APU throttles on its own frame clock */
    d->monitor.stream = NULL;
    d->monitor.queued_bytes_low = 0;
    d->monitor.queued_bytes_high = 0;
}

void mcpx_apu_monitor_finalize(MCPXAPUState *d)
{
}

void mcpx_apu_monitor_frame(MCPXAPUState *d)
{
    /* same cadence as monitor.c: one 256-frame buffer every 8 EP frames */
    if ((d->ep_frame_div + 1) % 8) {
        return;
    }

    WasmAudioRing *ring = &wasm_audio_ring;
    const uint32_t n = ARRAY_SIZE(d->monitor.frame_buf);
    uint32_t w = __atomic_load_n(&ring->write, __ATOMIC_RELAXED);

    /* only the consumer writes `read`; if it has fallen a whole ring behind
     * it notices (write - read > capacity) and skips ahead */
    for (uint32_t i = 0; i < n; i++) {
        uint32_t slot = (w + i) & (ring->capacity - 1);
        ring->data[slot * 2] = d->monitor.frame_buf[i][0];
        ring->data[slot * 2 + 1] = d->monitor.frame_buf[i][1];
    }
    __atomic_store_n(&ring->write, w + n, __ATOMIC_RELEASE);

    memset(d->monitor.frame_buf, 0, sizeof(d->monitor.frame_buf));
}
