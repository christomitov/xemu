/*
 * Geforce NV2A PGRAPH WebGPU Renderer (browser / emscripten)
 *
 * Runs entirely on the nv2a.pfifo_thread pthread: the page's <canvas
 * id="canvas"> is transferred to that worker as an OffscreenCanvas when the
 * thread is created (see qemu_thread_create), and all WebGPU objects live
 * there. Async WebGPU operations are waited on with wgpuInstanceWaitAny,
 * which suspends via Asyncify.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#include "renderer.h"
#include "qapi/error.h"

static void on_adapter(WGPURequestAdapterStatus status, WGPUAdapter adapter,
                       WGPUStringView msg, void *u1, void *u2)
{
    PGRAPHWgpuState *r = u1;
    if (status == WGPURequestAdapterStatus_Success) {
        r->adapter = adapter;
    } else {
        fprintf(stderr, "[wgpu] requestAdapter failed: %.*s\n",
                (int)msg.length, msg.data);
    }
}

static void on_device(WGPURequestDeviceStatus status, WGPUDevice device,
                      WGPUStringView msg, void *u1, void *u2)
{
    PGRAPHWgpuState *r = u1;
    if (status == WGPURequestDeviceStatus_Success) {
        r->device = device;
    } else {
        fprintf(stderr, "[wgpu] requestDevice failed: %.*s\n",
                (int)msg.length, msg.data);
    }
}

static void on_uncaptured_error(const WGPUDevice *device, WGPUErrorType type,
                                WGPUStringView msg, void *u1, void *u2)
{
    static int count;
    if (count++ < 50) {
        fprintf(stderr, "[wgpu] error %d: %.*s\n", type, (int)msg.length,
                msg.data);
    }
}

static void on_device_lost(const WGPUDevice *device, WGPUDeviceLostReason reason,
                           WGPUStringView msg, void *u1, void *u2)
{
    fprintf(stderr, "[wgpu] device lost (%d): %.*s\n", reason,
            (int)msg.length, msg.data);
}

void pgraph_wgpu_wait(PGRAPHWgpuState *r, WGPUFuture future)
{
    WGPUFutureWaitInfo info = { .future = future };
    wgpuInstanceWaitAny(r->instance, 1, &info, UINT64_MAX);
}

WGPUShaderModule pgraph_wgpu_create_wgsl_module(PGRAPHWgpuState *r,
                                               const char *label,
                                               const char *wgsl)
{
    WGPUShaderSourceWGSL src = {
        .chain = { .sType = WGPUSType_ShaderSourceWGSL },
        .code = { wgsl, WGPU_STRLEN },
    };
    WGPUShaderModuleDescriptor desc = {
        .nextInChain = &src.chain,
        .label = { label, WGPU_STRLEN },
    };
    return wgpuDeviceCreateShaderModule(r->device, &desc);
}

static void init_device(PGRAPHWgpuState *r, Error **errp)
{
    WGPUInstanceFeatureName feats[] = { WGPUInstanceFeatureName_TimedWaitAny };
    WGPUInstanceDescriptor idesc = {
        .requiredFeatureCount = 1,
        .requiredFeatures = feats,
    };
    fprintf(stderr, "[wgpu] init: creating instance\n");
    r->instance = wgpuCreateInstance(&idesc);
    fprintf(stderr, "[wgpu] init: instance=%p, requesting adapter\n", (void *)r->instance);
    if (!r->instance) {
        error_setg(errp, "WebGPU: wgpuCreateInstance failed");
        return;
    }

    WGPURequestAdapterOptions aopts = {
        .powerPreference = WGPUPowerPreference_HighPerformance,
    };
    pgraph_wgpu_wait(r, wgpuInstanceRequestAdapter(
        r->instance, &aopts,
        (WGPURequestAdapterCallbackInfo){ .mode = WGPUCallbackMode_WaitAnyOnly,
                                          .callback = on_adapter,
                                          .userdata1 = r }));
    fprintf(stderr, "[wgpu] init: adapter=%p\n", (void *)r->adapter);
    if (!r->adapter) {
        error_setg(errp, "WebGPU: no adapter (is WebGPU enabled?)");
        return;
    }

    WGPUDeviceDescriptor ddesc = {
        .label = { "nv2a", WGPU_STRLEN },
        .deviceLostCallbackInfo = { .mode = WGPUCallbackMode_AllowSpontaneous,
                                    .callback = on_device_lost },
        .uncapturedErrorCallbackInfo = { .callback = on_uncaptured_error },
    };
    pgraph_wgpu_wait(r, wgpuAdapterRequestDevice(
        r->adapter, &ddesc,
        (WGPURequestDeviceCallbackInfo){ .mode = WGPUCallbackMode_WaitAnyOnly,
                                         .callback = on_device,
                                         .userdata1 = r }));
    if (!r->device) {
        error_setg(errp, "WebGPU: requestDevice failed");
        return;
    }
    r->queue = wgpuDeviceGetQueue(r->device);
    fprintf(stderr, "[wgpu] init: device=%p, creating surface\n", (void *)r->device);

    WGPUEmscriptenSurfaceSourceCanvasHTMLSelector sel = {
        .chain = { .sType = WGPUSType_EmscriptenSurfaceSourceCanvasHTMLSelector },
        .selector = { "#canvas", WGPU_STRLEN },
    };
    WGPUSurfaceDescriptor sdesc = { .nextInChain = &sel.chain };
    r->surface = wgpuInstanceCreateSurface(r->instance, &sdesc);
    if (!r->surface) {
        error_setg(errp, "WebGPU: cannot create canvas surface");
        return;
    }

    WGPUSurfaceCapabilities caps = { 0 };
    wgpuSurfaceGetCapabilities(r->surface, r->adapter, &caps);
    r->surface_format = caps.formatCount ? caps.formats[0]
                                         : WGPUTextureFormat_BGRA8Unorm;
    wgpuSurfaceCapabilitiesFreeMembers(caps);

    fprintf(stderr, "[wgpu] device ready, surface format %d\n",
            r->surface_format);
}

static void pgraph_wgpu_init(NV2AState *d, Error **errp)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHWgpuState *r = g_malloc0(sizeof(*r));
    pg->wgpu_renderer_state = r;

    init_device(r, errp);
    if (*errp) {
        return;
    }
    pgraph_wgpu_init_display(pg);
}

static void pgraph_wgpu_finalize(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;

    if (!r) {
        return;
    }
    pgraph_wgpu_finalize_display(pg);
    if (r->surface) {
        wgpuSurfaceRelease(r->surface);
    }
    if (r->queue) {
        wgpuQueueRelease(r->queue);
    }
    if (r->device) {
        wgpuDeviceRelease(r->device);
    }
    if (r->adapter) {
        wgpuAdapterRelease(r->adapter);
    }
    if (r->instance) {
        wgpuInstanceRelease(r->instance);
    }
    g_free(r);
    pg->wgpu_renderer_state = NULL;
}

static void pgraph_wgpu_sync(NV2AState *d)
{
    pgraph_wgpu_render_display(d);
    qatomic_set(&d->pgraph.sync_pending, false);
    qemu_event_set(&d->pgraph.sync_complete);
}

static void pgraph_wgpu_flush(NV2AState *d)
{
    qatomic_set(&d->pgraph.flush_pending, false);
    qemu_event_set(&d->pgraph.flush_complete);
}

static void pgraph_wgpu_process_pending(NV2AState *d)
{
    if (qatomic_read(&d->pgraph.sync_pending) ||
        qatomic_read(&d->pgraph.flush_pending)) {
        qemu_mutex_unlock(&d->pfifo.lock);
        qemu_mutex_lock(&d->pgraph.lock);
        if (qatomic_read(&d->pgraph.sync_pending)) {
            pgraph_wgpu_sync(d);
        }
        if (qatomic_read(&d->pgraph.flush_pending)) {
            pgraph_wgpu_flush(d);
        }
        qemu_mutex_unlock(&d->pgraph.lock);
        qemu_mutex_lock(&d->pfifo.lock);
    }
}

/* Not implemented yet: draw path (surfaces, pipelines, textures, reports). */
static void pgraph_wgpu_nop(NV2AState *d)
{
}

static void pgraph_wgpu_clear_surface(NV2AState *d, uint32_t parameter)
{
}

static void pgraph_wgpu_get_report(NV2AState *d, uint32_t parameter)
{
    pgraph_write_zpass_pixel_cnt_report(d, parameter, 0);
}

static void pgraph_wgpu_surface_update(NV2AState *d, bool upload,
                                       bool color_write, bool zeta_write)
{
}

static PGRAPHRenderer pgraph_wgpu_renderer = {
    .type = CONFIG_DISPLAY_RENDERER_WEBGPU,
    .name = "WebGPU",
    .ops = {
        .init = pgraph_wgpu_init,
        .finalize = pgraph_wgpu_finalize,
        .clear_report_value = pgraph_wgpu_nop,
        .clear_surface = pgraph_wgpu_clear_surface,
        .draw_begin = pgraph_wgpu_nop,
        .draw_end = pgraph_wgpu_nop,
        .flip_stall = pgraph_wgpu_nop,
        .flush_draw = pgraph_wgpu_nop,
        .get_report = pgraph_wgpu_get_report,
        .image_blit = pgraph_wgpu_nop,
        .pre_savevm_trigger = pgraph_wgpu_nop,
        .pre_savevm_wait = pgraph_wgpu_nop,
        .pre_shutdown_trigger = pgraph_wgpu_nop,
        .pre_shutdown_wait = pgraph_wgpu_nop,
        .process_pending = pgraph_wgpu_process_pending,
        .process_pending_reports = pgraph_wgpu_nop,
        .surface_update = pgraph_wgpu_surface_update,
    }
};

static void __attribute__((constructor)) register_renderer(void)
{
    pgraph_renderer_register(&pgraph_wgpu_renderer);
}
