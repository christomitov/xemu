/*
 * Geforce NV2A PGRAPH WebGPU Renderer: command encoding
 *
 * Port of vk/command.c and the command-buffer parts of vk/draw.c. Exactly one
 * WGPUCommandEncoder is open between finishes. Render passes are
 * WGPURenderPassEncoders on it; other modules get the encoder (with no pass
 * active) via pgraph_wgpu_begin_nondraw_commands() for copies / compute and
 * may record their own passes on it.
 *
 * Copyright (c) 2024-2025 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#include "renderer.h"

static const enum NV2A_PROF_COUNTERS_ENUM finish_reason_to_counter_enum[] = {
    [WGPU_FINISH_REASON_VERTEX_BUFFER_DIRTY] =
        NV2A_PROF_FINISH_VERTEX_BUFFER_DIRTY,
    [WGPU_FINISH_REASON_SURFACE_CREATE] = NV2A_PROF_FINISH_SURFACE_CREATE,
    [WGPU_FINISH_REASON_SURFACE_DOWN] = NV2A_PROF_FINISH_SURFACE_DOWN,
    [WGPU_FINISH_REASON_NEED_BUFFER_SPACE] = NV2A_PROF_FINISH_NEED_BUFFER_SPACE,
    [WGPU_FINISH_REASON_FRAMEBUFFER_DIRTY] = NV2A_PROF_FINISH_FRAMEBUFFER_DIRTY,
    [WGPU_FINISH_REASON_PRESENTING] = NV2A_PROF_FINISH_PRESENTING,
    [WGPU_FINISH_REASON_FLIP_STALL] = NV2A_PROF_FINISH_FLIP_STALL,
    [WGPU_FINISH_REASON_FLUSH] = NV2A_PROF_FINISH_FLUSH,
    [WGPU_FINISH_REASON_STALLED] = NV2A_PROF_FINISH_STALLED,
};

void pgraph_wgpu_ensure_command_buffer(PGRAPHState *pg)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;

    if (r->draw.in_command_buffer) {
        return;
    }

    WGPUCommandEncoderDescriptor desc = {
        .label = { "nv2a", WGPU_STRLEN },
    };
    r->draw.encoder = wgpuDeviceCreateCommandEncoder(r->device, &desc);
    r->draw.command_buffer_start_time = pg->draw_time;
    r->draw.in_command_buffer = true;
}

void pgraph_wgpu_begin_render_pass(PGRAPHState *pg,
                                   const WGPUColor *color_clear,
                                   const float *depth_clear,
                                   const uint32_t *stencil_clear)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;

    assert(r->draw.in_command_buffer);
    assert(!r->draw.in_render_pass);
    assert(r->color_binding || r->zeta_binding);

    nv2a_profile_inc_counter(NV2A_PROF_PIPELINE_RENDERPASSES);

    WGPURenderPassColorAttachment color = { 0 };
    WGPURenderPassDepthStencilAttachment zeta = { 0 };
    WGPURenderPassDescriptor desc = {
        .label = { "nv2a-draw", WGPU_STRLEN },
        .occlusionQuerySet = r->draw.query_set,
    };

    SurfaceBinding *binding = r->color_binding ?: r->zeta_binding;
    uint32_t width = wgpuTextureGetWidth(binding->texture);
    uint32_t height = wgpuTextureGetHeight(binding->texture);

    if (r->color_binding) {
        color = (WGPURenderPassColorAttachment){
            .view = r->color_binding->view,
            .depthSlice = WGPU_DEPTH_SLICE_UNDEFINED,
            .loadOp = color_clear ? WGPULoadOp_Clear : WGPULoadOp_Load,
            .storeOp = WGPUStoreOp_Store,
        };
        if (color_clear) {
            color.clearValue = *color_clear;
        }
        desc.colorAttachmentCount = 1;
        desc.colorAttachments = &color;
    }

    if (r->zeta_binding) {
        const WgpuSurfaceFormatInfo *f = &r->zeta_binding->host_fmt;
        zeta.view = r->zeta_binding->view;
        if (f->depth) {
            zeta.depthLoadOp = depth_clear ? WGPULoadOp_Clear : WGPULoadOp_Load;
            zeta.depthStoreOp = WGPUStoreOp_Store;
            zeta.depthClearValue = depth_clear ? *depth_clear : 1.0f;
        }
        if (f->stencil) {
            zeta.stencilLoadOp =
                stencil_clear ? WGPULoadOp_Clear : WGPULoadOp_Load;
            zeta.stencilStoreOp = WGPUStoreOp_Store;
            zeta.stencilClearValue = stencil_clear ? *stencil_clear : 0;
        }
        desc.depthStencilAttachment = &zeta;

        if (r->color_binding) {
            uint32_t zw = wgpuTextureGetWidth(r->zeta_binding->texture);
            uint32_t zh = wgpuTextureGetHeight(r->zeta_binding->texture);
            if (zw != width || zh != height) {
                static bool logged;
                if (!logged) {
                    fprintf(stderr, "[wgpu] color/zeta attachment size "
                            "mismatch: %ux%u vs %ux%u\n",
                            width, height, zw, zh);
                    logged = true;
                }
                width = MIN(width, zw);
                height = MIN(height, zh);
            }
        }
    }

    if (!r->zeta_binding) {
        zeta = (WGPURenderPassDepthStencilAttachment){
            .view = pgraph_wgpu_scratch_zeta_view(r, width, height),
            .depthLoadOp = WGPULoadOp_Clear,
            .depthStoreOp = WGPUStoreOp_Discard,
            .depthClearValue = 1.0f,
        };
        desc.depthStencilAttachment = &zeta;
    }

    r->draw.pass = wgpuCommandEncoderBeginRenderPass(r->draw.encoder, &desc);
    r->draw.in_render_pass = true;
    r->draw.pass_color_view = r->color_binding ? r->color_binding->view : NULL;
    r->draw.pass_zeta_view = r->zeta_binding ? r->zeta_binding->view : NULL;
    r->draw.pass_width = width;
    r->draw.pass_height = height;
    r->draw.gpu_query_active = false;
    r->draw.pass_pipeline = NULL;
}

/* Throwaway depth target for passes without a zeta surface, grown to fit. */
WGPUTextureView pgraph_wgpu_scratch_zeta_view(PGRAPHWgpuState *r,
                                              uint32_t width, uint32_t height)
{
    PGRAPHWgpuDrawState *ds = &r->draw;

    if (ds->scratch_zeta && ds->scratch_zeta_w == width &&
        ds->scratch_zeta_h == height) {
        return ds->scratch_zeta_view;
    }
    if (ds->scratch_zeta_view) {
        wgpuTextureViewRelease(ds->scratch_zeta_view);
    }
    if (ds->scratch_zeta) {
        wgpuTextureRelease(ds->scratch_zeta);
    }
    ds->scratch_zeta = wgpuDeviceCreateTexture(
        r->device, &(WGPUTextureDescriptor){
                       .label = { "scratch zeta", WGPU_STRLEN },
                       .usage = WGPUTextureUsage_RenderAttachment,
                       .dimension = WGPUTextureDimension_2D,
                       .size = { width, height, 1 },
                       .format = WGPUTextureFormat_Depth32Float,
                       .mipLevelCount = 1,
                       .sampleCount = 1 });
    ds->scratch_zeta_view = wgpuTextureCreateView(ds->scratch_zeta, NULL);
    ds->scratch_zeta_w = width;
    ds->scratch_zeta_h = height;
    return ds->scratch_zeta_view;
}

void pgraph_wgpu_end_render_pass(PGRAPHState *pg)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;

    if (!r->draw.in_render_pass) {
        return;
    }
    if (r->draw.gpu_query_active) {
        pgraph_wgpu_end_gpu_query(pg);
    }
    wgpuRenderPassEncoderEnd(r->draw.pass);
    wgpuRenderPassEncoderRelease(r->draw.pass);
    r->draw.pass = NULL;
    r->draw.in_render_pass = false;
    r->draw.pass_color_view = NULL;
    r->draw.pass_zeta_view = NULL;
}

void pgraph_wgpu_ensure_not_in_render_pass(PGRAPHState *pg)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;

    pgraph_wgpu_end_render_pass(pg);
    if (r->draw.query_in_flight) {
        pgraph_wgpu_end_query(pg);
    }
}

WGPUCommandEncoder pgraph_wgpu_begin_nondraw_commands(PGRAPHState *pg)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;

    pgraph_wgpu_ensure_command_buffer(pg);
    pgraph_wgpu_ensure_not_in_render_pass(pg);
    return r->draw.encoder;
}

void pgraph_wgpu_end_nondraw_commands(PGRAPHState *pg, WGPUCommandEncoder enc)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;
    assert(enc == r->draw.encoder);
    assert(!r->draw.in_render_pass);
}

void pgraph_wgpu_finish(PGRAPHState *pg, FinishReason finish_reason)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;
    PGRAPHWgpuDrawState *ds = &r->draw;

    assert(!ds->in_draw);

    if (ds->in_command_buffer) {
        nv2a_profile_inc_counter(finish_reason_to_counter_enum[finish_reason]);

        pgraph_wgpu_end_render_pass(pg);
        if (ds->query_in_flight) {
            pgraph_wgpu_end_query(pg);
        }
        pgraph_wgpu_resolve_queries(pg);

        WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(ds->encoder, NULL);
        wgpuCommandEncoderRelease(ds->encoder);
        ds->encoder = NULL;
        ds->in_command_buffer = false;

        nv2a_profile_inc_counter(NV2A_PROF_QUEUE_SUBMIT);
        wgpuQueueSubmit(r->queue, 1, &cmd);
        wgpuCommandBufferRelease(cmd);
        ds->submit_count += 1;
        pgraph_wgpu_shaders_on_submit(pg);

        /* mirrors vkWaitForFences on the command buffer fence */
        pgraph_wgpu_wait_queue_idle(r);

        /* everything recorded has executed: append-only buffers restart */
        ds->storage_buffers[WGPU_BUFFER_INDEX].buffer_offset = 0;
        ds->storage_buffers[WGPU_BUFFER_VERTEX_INLINE].buffer_offset = 0;
        if (ds->uploaded_bitmap) {
            bitmap_clear(ds->uploaded_bitmap, 0, ds->bitmap_size);
        }
        ds->num_gpu_queries = 0;
        ds->gpu_query_active = false;
    }

    NV2AState *d = container_of(pg, NV2AState, pgraph);
    pgraph_wgpu_process_pending_reports_internal(d);
}
