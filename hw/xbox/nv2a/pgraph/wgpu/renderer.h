/*
 * Geforce NV2A PGRAPH WebGPU Renderer (browser / emscripten)
 *
 * Port of the Vulkan renderer (../vk) to WebGPU (emdawnwebgpu, webgpu.h).
 * Everything runs on nv2a.pfifo_thread; the page canvas is an OffscreenCanvas
 * owned by that worker. Async WebGPU results (adapter/device, buffer maps,
 * queue completion) are waited on with pgraph_wgpu_wait(), which suspends
 * via Asyncify.
 *
 * Module ownership (each module owns its own header + .c files; cross-module
 * calls go only through the prototypes declared here):
 *   core     renderer.c display.c            (this header)
 *   draw     draw.c vertex.c buffer.c command.c reports.c   draw.h
 *   surface  surface.c surface-{compute,reshape}.c blit.c   surface.h
 *   texture  texture.c                                      texture.h
 *   shaders  shaders.c glsl.c                               shaders.h
 *
 * WebGPU vs Vulkan, in short:
 *  - no image layouts / barriers / VMA: textures and buffers are just objects
 *  - one WGPUCommandEncoder is open between finishes (draw.h: command state);
 *    render passes are WGPURenderPassEncoders on it
 *  - uploads use wgpuQueueWriteBuffer/WriteTexture; downloads copy into a
 *    MapRead buffer and pgraph_wgpu_read_buffer_sync()
 *  - no geometry shaders: primitives are converted to indexed lists on the
 *    CPU (draw module), provoking vertex first (WGSL flat = first vertex)
 *  - shaders: xemu's GLSL generator -> glslang -> SPIR-V -> Tint -> WGSL
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#ifndef HW_XBOX_NV2A_PGRAPH_WGPU_RENDERER_H
#define HW_XBOX_NV2A_PGRAPH_WGPU_RENDERER_H

#include "qemu/osdep.h"
#include "qemu/thread.h"
#include "qemu/queue.h"
#include "qemu/lru.h"
#include "hw/hw.h"
#include "hw/xbox/nv2a/nv2a_int.h"
#include "hw/xbox/nv2a/nv2a_regs.h"
#include "hw/xbox/nv2a/pgraph/surface.h"
#include "hw/xbox/nv2a/pgraph/texture.h"
#include "hw/xbox/nv2a/pgraph/glsl/shaders.h"

#include <webgpu/webgpu.h>

typedef struct PGRAPHWgpuState PGRAPHWgpuState;
/* cross-module types, defined in their owning module header */
typedef struct SurfaceBinding SurfaceBinding;   /* surface.h */
typedef struct TextureBinding TextureBinding;   /* texture.h */
typedef struct ShaderBinding ShaderBinding;     /* shaders.h */

#include "draw.h"
#include "surface.h"
#include "texture.h"
#include "shaders.h"

/* ---- core: display (display.c) ---- */
typedef struct PGRAPHWgpuDisplayState {
    WGPURenderPipeline pipeline;
    WGPUBindGroupLayout bind_group_layout;
    WGPUSampler sampler;

    /* fallback scanout source: VRAM copy of the current framebuffer, used
     * when no color SurfaceBinding covers the CRTC start address */
    WGPUTexture texture;
    WGPUTextureView view;
    WGPUBindGroup bind_group;
    int width, height;
    uint32_t *conv; /* 15/16bpp -> BGRA8 staging */
    size_t conv_size;

    WGPUBuffer xform; /* vec4 uv scale/offset for the displayed region */
    bool need_yield; /* presented: yield to the browser once unlocked */
    /* bind group for the render surface presented last frame */
    WGPUBindGroup surface_bind_group;
    WGPUTextureView surface_bound_view;
    float xform_values[4];
    /* what was presented last (to skip re-presenting an unchanged frame) */
    SurfaceBinding *last_surface;
    unsigned int last_draw_time, last_frame_time;
    hwaddr last_scanout;

    /* PVIDEO video overlay (YUY2 converted to RGBA on the CPU) */
    WGPUTexture pvideo_texture;
    WGPUTextureView pvideo_view;
    int pvideo_width, pvideo_height;
    uint8_t *pvideo_conv;
    size_t pvideo_conv_size;
} PGRAPHWgpuDisplayState;

typedef struct PGRAPHWgpuState {
    /* core device state (renderer.c) */
    WGPUInstance instance;
    WGPUAdapter adapter;
    WGPUDevice device;
    WGPUQueue queue;
    WGPULimits limits;
    bool has_depth32_stencil8;   /* WGPUFeatureName_Depth32FloatStencil8 */
    bool has_bc_compression;     /* WGPUFeatureName_TextureCompressionBC */
    bool has_unclipped_depth;    /* WGPUFeatureName_DepthClipControl */
    bool has_float32_filterable; /* WGPUFeatureName_Float32Filterable */

    WGPUSurface surface;
    WGPUTextureFormat surface_format;
    int surface_width, surface_height;

    PGRAPHWgpuDisplayState display;

    /* module state, each owned by its module */
    PGRAPHWgpuDrawState draw;       /* draw.h */
    PGRAPHWgpuSurfaceState surf;    /* surface.h */
    PGRAPHWgpuTextureState tex;     /* texture.h */
    PGRAPHWgpuShaderState shaders;  /* shaders.h */

    /* shared cross-module state (read by several modules) */
    SurfaceBinding *color_binding, *zeta_binding; /* set by surface module */
    bool framebuffer_dirty;        /* surface -> draw: attachments changed */
    bool texture_bindings_changed; /* texture -> draw/shaders */
    bool shader_bindings_changed;  /* shaders -> draw */
} PGRAPHWgpuState;

/* ---- core: renderer.c ---- */
void pgraph_wgpu_wait_at(PGRAPHWgpuState *r, WGPUFuture future,
                         const char *where);
/* where: call site, for the waitms:* report counters */
#define pgraph_wgpu_wait(r, future) \
    pgraph_wgpu_wait_at((r), (future), __FILE__ ":" stringify(__LINE__))
WGPUShaderModule pgraph_wgpu_create_wgsl_module(PGRAPHWgpuState *r,
                                               const char *label,
                                               const char *wgsl);
/* Blocking readback: map buffer [offset, offset+size) for reading (it must
 * have WGPUBufferUsage_MapRead), copy into dst, unmap. The caller must have
 * submitted the commands that fill it (pgraph_wgpu_finish). */
void pgraph_wgpu_read_buffer_sync_at(PGRAPHWgpuState *r, WGPUBuffer buffer,
                                     size_t offset, size_t size, void *dst,
                                     const char *where);
#define pgraph_wgpu_read_buffer_sync(r, buffer, offset, size, dst)          \
    pgraph_wgpu_read_buffer_sync_at((r), (buffer), (offset), (size), (dst), \
                                    __FILE__ ":" stringify(__LINE__))
/* Wait until all submitted GPU work has completed. */
void pgraph_wgpu_wait_queue_idle(PGRAPHWgpuState *r);

/* ---- core: display.c ---- */
void pgraph_wgpu_init_display(PGRAPHState *pg);
void pgraph_wgpu_finalize_display(PGRAPHState *pg);
void pgraph_wgpu_render_display(NV2AState *d);

/* ---- draw module (draw.c vertex.c buffer.c command.c reports.c) ---- */
typedef enum FinishReason {
    WGPU_FINISH_REASON_VERTEX_BUFFER_DIRTY,
    WGPU_FINISH_REASON_SURFACE_CREATE,
    WGPU_FINISH_REASON_SURFACE_DOWN,
    WGPU_FINISH_REASON_NEED_BUFFER_SPACE,
    WGPU_FINISH_REASON_FRAMEBUFFER_DIRTY,
    WGPU_FINISH_REASON_PRESENTING,
    WGPU_FINISH_REASON_FLIP_STALL,
    WGPU_FINISH_REASON_FLUSH,
    WGPU_FINISH_REASON_STALLED,
} FinishReason;

void pgraph_wgpu_init_pipelines(PGRAPHState *pg);
void pgraph_wgpu_finalize_pipelines(PGRAPHState *pg);
void pgraph_wgpu_init_buffers(NV2AState *d);
void pgraph_wgpu_finalize_buffers(NV2AState *d);
void pgraph_wgpu_init_reports(PGRAPHState *pg);
void pgraph_wgpu_finalize_reports(PGRAPHState *pg);

void pgraph_wgpu_clear_surface(NV2AState *d, uint32_t parameter);
void pgraph_wgpu_draw_begin(NV2AState *d);
void pgraph_wgpu_draw_end(NV2AState *d);
void pgraph_wgpu_flush_draw(NV2AState *d);
/* End any render pass, submit the open encoder and wait for completion. */
void pgraph_wgpu_finish(PGRAPHState *pg, FinishReason why);
/* Ensure an encoder is open and no render pass is active; returns it. Use
 * for copies/compute from other modules. */
WGPUCommandEncoder pgraph_wgpu_begin_nondraw_commands(PGRAPHState *pg);
void pgraph_wgpu_end_nondraw_commands(PGRAPHState *pg, WGPUCommandEncoder enc);
void pgraph_wgpu_ensure_not_in_render_pass(PGRAPHState *pg);
void pgraph_wgpu_set_surface_dirty(PGRAPHState *pg, bool color, bool zeta);

void pgraph_wgpu_update_vertex_ram_buffer(PGRAPHState *pg, hwaddr offset,
                                          void *data, size_t size);
void pgraph_wgpu_clear_report_value(NV2AState *d);
void pgraph_wgpu_get_report(NV2AState *d, uint32_t parameter);
void pgraph_wgpu_process_pending_reports(NV2AState *d);
void pgraph_wgpu_process_pending_reports_internal(NV2AState *d);

/* ---- surface module (surface.c surface-compute.c blit.c) ---- */
void pgraph_wgpu_init_surfaces(PGRAPHState *pg);
void pgraph_wgpu_finalize_surfaces(PGRAPHState *pg);
void pgraph_wgpu_surface_flush(NV2AState *d);
void pgraph_wgpu_surface_update(NV2AState *d, bool upload, bool color_write,
                                bool zeta_write);
void pgraph_wgpu_process_pending_downloads(NV2AState *d);
void pgraph_wgpu_download_dirty_surfaces(NV2AState *d);
/*
 * Why the next surface download happens (static or g_intern_string()'d);
 * set by callers on the pfifo thread, counted per reason for the Stats report.
 */
extern const char *pgraph_wgpu_dl_reason;
/* NV097_CLEAR_SURFACE parameter while a clear binds its surfaces, else 0 */
extern uint32_t pgraph_wgpu_clear_param;
void pgraph_wgpu_surface_download_if_dirty(NV2AState *d,
                                           SurfaceBinding *surface);
void pgraph_wgpu_download_surfaces_in_range_if_dirty(PGRAPHState *pg,
                                                     hwaddr start,
                                                     hwaddr size);
void pgraph_wgpu_wait_for_surface_download(SurfaceBinding *surface);
void pgraph_wgpu_upload_surface_data(NV2AState *d, SurfaceBinding *surface,
                                     bool force);
SurfaceBinding *pgraph_wgpu_surface_get(NV2AState *d, hwaddr addr);
SurfaceBinding *pgraph_wgpu_surface_get_within(NV2AState *d, hwaddr addr);
void pgraph_wgpu_set_surface_scale_factor(NV2AState *d, unsigned int scale);
unsigned int pgraph_wgpu_get_surface_scale_factor(NV2AState *d);
void pgraph_wgpu_reload_surface_scale_factor(PGRAPHState *pg);
void pgraph_wgpu_image_blit(NV2AState *d);

/* ---- texture module (texture.c) ---- */
void pgraph_wgpu_init_textures(PGRAPHState *pg);
void pgraph_wgpu_finalize_textures(PGRAPHState *pg);
/* Resolve NV2A texture state for all 4 stages into
 * r->tex.texture_bindings[]; sets r->texture_bindings_changed. */
void pgraph_wgpu_bind_textures(NV2AState *d);
void pgraph_wgpu_mark_textures_possibly_dirty(NV2AState *d, hwaddr addr,
                                              hwaddr size);
void pgraph_wgpu_trim_texture_cache(PGRAPHState *pg);

/* ---- shaders module (shaders.c glsl.c) ---- */
void pgraph_wgpu_init_shaders(PGRAPHState *pg);
void pgraph_wgpu_finalize_shaders(PGRAPHState *pg);
/* Resolve r->shaders.shader_binding for the current state (generating and
 * compiling WGSL on cache miss); sets r->shader_bindings_changed. */
void pgraph_wgpu_bind_shaders(PGRAPHState *pg);
/* Upload uniforms for the current draw and return a bind group for group 0
 * (uniform buffers + the 4 texture stages from r->tex.texture_bindings). */
WGPUBindGroup pgraph_wgpu_update_bind_group(PGRAPHState *pg);
void pgraph_wgpu_uniform_offsets(PGRAPHState *pg, uint32_t out[2]);
void pgraph_wgpu_flush_uniforms(PGRAPHWgpuState *r);

#endif
