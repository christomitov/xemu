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
#include "qemu/xemu-wasm-stats.h"
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

    WGPUFeatureName wanted[] = {
        WGPUFeatureName_Depth32FloatStencil8,
        WGPUFeatureName_TextureCompressionBC,
        WGPUFeatureName_DepthClipControl,
        WGPUFeatureName_Float32Filterable,
    };
    WGPUFeatureName feats_on[ARRAY_SIZE(wanted)];
    size_t nfeats = 0;
    for (size_t i = 0; i < ARRAY_SIZE(wanted); i++) {
        if (wgpuAdapterHasFeature(r->adapter, wanted[i])) {
            feats_on[nfeats++] = wanted[i];
        }
    }
    r->has_depth32_stencil8 =
        wgpuAdapterHasFeature(r->adapter, WGPUFeatureName_Depth32FloatStencil8);
    r->has_bc_compression =
        wgpuAdapterHasFeature(r->adapter, WGPUFeatureName_TextureCompressionBC);
    r->has_unclipped_depth =
        wgpuAdapterHasFeature(r->adapter, WGPUFeatureName_DepthClipControl);
    r->has_float32_filterable =
        wgpuAdapterHasFeature(r->adapter, WGPUFeatureName_Float32Filterable);

    /* ask for the adapter's limits (defaults are too small for a 64 MiB
     * VRAM mirror buffer on some adapters) */
    WGPULimits adapter_limits = WGPU_LIMITS_INIT;
    wgpuAdapterGetLimits(r->adapter, &adapter_limits);

    WGPUDeviceDescriptor ddesc = {
        .label = { "nv2a", WGPU_STRLEN },
        .requiredFeatureCount = nfeats,
        .requiredFeatures = feats_on,
        .requiredLimits = &adapter_limits,
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
    r->limits = (WGPULimits)WGPU_LIMITS_INIT;
    wgpuDeviceGetLimits(r->device, &r->limits);
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

    fprintf(stderr, "[wgpu] device ready, surface format %d, features: "
            "d32s8=%d bc=%d unclipped_depth=%d f32filter=%d\n",
            r->surface_format, r->has_depth32_stencil8, r->has_bc_compression,
            r->has_unclipped_depth, r->has_float32_filterable);
}

static void on_buffer_mapped(WGPUMapAsyncStatus status, WGPUStringView msg,
                             void *u1, void *u2)
{
    if (status != WGPUMapAsyncStatus_Success) {
        fprintf(stderr, "[wgpu] mapAsync failed (%d): %.*s\n", status,
                (int)msg.length, msg.data);
    }
    *(bool *)u1 = status == WGPUMapAsyncStatus_Success;
}

void pgraph_wgpu_read_buffer_sync(PGRAPHWgpuState *r, WGPUBuffer buffer,
                                  size_t offset, size_t size, void *dst)
{
    bool ok = false;
    XSTAT_INC(n_readback);
    XSTAT_T0();
    pgraph_wgpu_wait(r, wgpuBufferMapAsync(
        buffer, WGPUMapMode_Read, offset, size,
        (WGPUBufferMapCallbackInfo){ .mode = WGPUCallbackMode_WaitAnyOnly,
                                     .callback = on_buffer_mapped,
                                     .userdata1 = &ok }));
    if (ok) {
        const void *src = wgpuBufferGetConstMappedRange(buffer, offset, size);
        memcpy(dst, src, size);
    } else {
        memset(dst, 0, size);
    }
    wgpuBufferUnmap(buffer);
    XSTAT_T1(ns_readback);
}

static void on_work_done(WGPUQueueWorkDoneStatus status, WGPUStringView msg,
                         void *u1, void *u2)
{
}

void pgraph_wgpu_wait_queue_idle(PGRAPHWgpuState *r)
{
    pgraph_wgpu_wait(r, wgpuQueueOnSubmittedWorkDone(
        r->queue, (WGPUQueueWorkDoneCallbackInfo){
                      .mode = WGPUCallbackMode_WaitAnyOnly,
                      .callback = on_work_done }));
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

    pgraph_wgpu_init_buffers(d);
    pgraph_wgpu_init_surfaces(pg);
    pgraph_wgpu_init_shaders(pg);
    pgraph_wgpu_init_pipelines(pg);
    pgraph_wgpu_init_textures(pg);
    pgraph_wgpu_init_reports(pg);
    pgraph_wgpu_init_display(pg);

    pgraph_wgpu_update_vertex_ram_buffer(pg, 0, d->vram_ptr,
                                         memory_region_size(d->vram));
}

static void pgraph_wgpu_finalize(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;

    if (!r) {
        return;
    }
    if (r->device) {
        pgraph_wgpu_finalize_display(pg);
        /* surfaces first: their flush may still record + submit work, which
         * needs the shader/pipeline/buffer state alive */
        pgraph_wgpu_finalize_surfaces(pg);
        pgraph_wgpu_finalize_reports(pg);
        pgraph_wgpu_finalize_textures(pg);
        pgraph_wgpu_finalize_pipelines(pg);
        pgraph_wgpu_finalize_shaders(pg);
        pgraph_wgpu_finalize_buffers(d);
    }
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

static void pgraph_wgpu_flush(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;

    pgraph_wgpu_finish(pg, WGPU_FINISH_REASON_FLUSH);
    pgraph_wgpu_surface_flush(d);
    pgraph_wgpu_mark_textures_possibly_dirty(d, 0, memory_region_size(d->vram));
    pgraph_wgpu_update_vertex_ram_buffer(pg, 0, d->vram_ptr,
                                         memory_region_size(d->vram));
    for (int i = 0; i < 4; i++) {
        pg->texture_dirty[i] = true;
    }

    qatomic_set(&d->pgraph.flush_pending, false);
    qemu_event_set(&d->pgraph.flush_complete);
}

static void pgraph_wgpu_sync(NV2AState *d)
{
    pgraph_wgpu_render_display(d);
    qatomic_set(&d->pgraph.sync_pending, false);
    qemu_event_set(&d->pgraph.sync_complete);
}

static void pgraph_wgpu_process_pending(NV2AState *d)
{
    PGRAPHWgpuState *r = d->pgraph.wgpu_renderer_state;

    if (qatomic_read(&r->surf.downloads_pending) ||
        qatomic_read(&r->surf.download_dirty_surfaces_pending) ||
        qatomic_read(&d->pgraph.sync_pending) ||
        qatomic_read(&d->pgraph.flush_pending)) {
        qemu_mutex_unlock(&d->pfifo.lock);
        qemu_mutex_lock(&d->pgraph.lock);
        if (qatomic_read(&r->surf.downloads_pending)) {
            pgraph_wgpu_process_pending_downloads(d);
        }
        if (qatomic_read(&r->surf.download_dirty_surfaces_pending)) {
            pgraph_wgpu_download_dirty_surfaces(d);
        }
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

static void pgraph_wgpu_flip_stall(NV2AState *d)
{
    extern volatile uint32_t xemu_wasm_flip_count;
    xemu_wasm_flip_count++;
    XSTAT_INC(n_flip);
    pgraph_wgpu_finish(&d->pgraph, WGPU_FINISH_REASON_FLIP_STALL);
}

static void pgraph_wgpu_pre_savevm_trigger(NV2AState *d)
{
    PGRAPHWgpuState *r = d->pgraph.wgpu_renderer_state;
    qatomic_set(&r->surf.download_dirty_surfaces_pending, true);
    qemu_event_reset(&r->surf.dirty_surfaces_download_complete);
}

static void pgraph_wgpu_pre_savevm_wait(NV2AState *d)
{
    PGRAPHWgpuState *r = d->pgraph.wgpu_renderer_state;
    qemu_event_wait(&r->surf.dirty_surfaces_download_complete);
}

static void pgraph_wgpu_nop(NV2AState *d)
{
}

static PGRAPHRenderer pgraph_wgpu_renderer = {
    .type = CONFIG_DISPLAY_RENDERER_WEBGPU,
    .name = "WebGPU",
    .ops = {
        .init = pgraph_wgpu_init,
        .finalize = pgraph_wgpu_finalize,
        .clear_report_value = pgraph_wgpu_clear_report_value,
        .clear_surface = pgraph_wgpu_clear_surface,
        .draw_begin = pgraph_wgpu_draw_begin,
        .draw_end = pgraph_wgpu_draw_end,
        .flip_stall = pgraph_wgpu_flip_stall,
        .flush_draw = pgraph_wgpu_flush_draw,
        .get_report = pgraph_wgpu_get_report,
        .image_blit = pgraph_wgpu_image_blit,
        .pre_savevm_trigger = pgraph_wgpu_pre_savevm_trigger,
        .pre_savevm_wait = pgraph_wgpu_pre_savevm_wait,
        .pre_shutdown_trigger = pgraph_wgpu_nop,
        .pre_shutdown_wait = pgraph_wgpu_nop,
        .process_pending = pgraph_wgpu_process_pending,
        .process_pending_reports = pgraph_wgpu_process_pending_reports,
        .surface_update = pgraph_wgpu_surface_update,
        .set_surface_scale_factor = pgraph_wgpu_set_surface_scale_factor,
        .get_surface_scale_factor = pgraph_wgpu_get_surface_scale_factor,
    }
};

static void __attribute__((constructor)) register_renderer(void)
{
    pgraph_renderer_register(&pgraph_wgpu_renderer);
}
