/*
 * Geforce NV2A PGRAPH WebGPU Renderer: surface module (owned by the surface module;
 * included from renderer.h, do not include directly)
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#ifndef HW_XBOX_NV2A_PGRAPH_WGPU_SURFACE_H
#define HW_XBOX_NV2A_PGRAPH_WGPU_SURFACE_H

typedef struct BasicSurfaceFormatInfo {
    unsigned int bytes_per_pixel;
} BasicSurfaceFormatInfo;

/*
 * How guest surface memory maps to the host texture. WebGPU has no 16-bit
 * packed color render targets, so R5G6B5 / X1R5G5B5 surfaces are rendered
 * into BGRA8Unorm and converted on the CPU at upload/download time.
 */
typedef enum WgpuSurfaceConversion {
    WGPU_SURFACE_CONV_NONE = 0,  /* guest bytes == host texel bytes */
    WGPU_SURFACE_CONV_R5G6B5,    /* guest R5G6B5   <-> host BGRA8Unorm */
    WGPU_SURFACE_CONV_X1R5G5B5,  /* guest A1R5G5B5 <-> host BGRA8Unorm */
    WGPU_SURFACE_CONV_Z24S8,     /* guest Z24S8    <-> host depth+stencil
                                    (packed/unpacked on the GPU, see
                                    surface-compute.c) */
} WgpuSurfaceConversion;

typedef struct WgpuSurfaceFormatInfo {
    WGPUTextureFormat format;           /* host texture format */
    unsigned int host_bytes_per_pixel;  /* bytes per texel of the host tex */
    bool depth, stencil;                /* aspects present */
    WgpuSurfaceConversion conv;
} WgpuSurfaceFormatInfo;

struct SurfaceBinding {
    QTAILQ_ENTRY(SurfaceBinding) entry;
    MemAccessCallback *access_cb;
    bool access_cb_write_only;  /* CPU copy current: only writes trapped */

    hwaddr vram_addr;

    SurfaceShape shape;
    uintptr_t dma_addr;
    uintptr_t dma_len;
    bool color;
    bool swizzle;

    unsigned int width;
    unsigned int height;
    unsigned int pitch;
    size_t size;

    /*
     * A private, full-size backing for the render image, at the same base.
     * Only this binding is live/trapped; it owns backing->size guest bytes.
     * The backing has no callback/list membership and cannot be recycled.
     * Before rebind/readback, merge this image by guest byte address (Morton
     * prefix, identical pitch, or tight BGRA linear reshape). Materialization
     * writes ALL backing bytes and releases it. No chains.
     */
    SurfaceBinding *backing;

    bool cleared;
    int frame_time;
    int draw_time;
    bool draw_dirty;
    bool download_pending;
    bool upload_pending;

    BasicSurfaceFormatInfo fmt;
    WgpuSurfaceFormatInfo host_fmt;

    /*
     * texture: RenderAttachment|TextureBinding|CopySrc|CopyDst.
     * view: all aspects; use as render attachment (and for sampling color
     *       surfaces / Depth16Unorm).
     * depth_view / stencil_view: single-aspect (DepthOnly / StencilOnly)
     *       views for sampling a zeta surface (WebGPU cannot sample an
     *       all-aspect view of a combined depth-stencil format). Both NULL
     *       for color surfaces; stencil_view NULL for Depth16Unorm.
     */
    WGPUTexture texture;
    WGPUTextureView view;
    WGPUTextureView depth_view;
    WGPUTextureView stencil_view;

    bool initialized;
};

typedef struct PGRAPHWgpuSurfaceState {
    /* required by core (renderer.c process_pending / savevm) */
    bool downloads_pending;
    QemuEvent downloads_complete;
    bool download_dirty_surfaces_pending;
    QemuEvent dirty_surfaces_download_complete;

    QTAILQ_HEAD(, SurfaceBinding) surfaces;
    QTAILQ_HEAD(, SurfaceBinding) invalid_surfaces;
    unsigned int num_retained;

    WgpuSurfaceFormatInfo kelvin_surface_zeta_map[3];

    /* host readback staging (CopyDst|MapRead), grown on demand */
    WGPUBuffer staging_dst;
    size_t staging_dst_size;

    /* surface-reshape.c: lazily created native BGRA linear byte reshaping */
    WGPUBindGroupLayout reshape_bgl;
    WGPURenderPipeline reshape_pipeline;

    /* surface-compute.c: depth/stencil pack (download) + unpack (upload) */
    struct {
        /* built for the host Z24S8 format chosen at init
         * (kelvin_surface_zeta_map[Z24S8].format) */
        WGPUBindGroupLayout pack_bgl;
        WGPUComputePipeline pack_pipeline;
        WGPUBindGroupLayout unpack_bgl;
        WGPURenderPipeline unpack_pipeline;
        WGPUBuffer pack_dst;    /* Storage|CopySrc: packed Z24S8 output */
        size_t pack_dst_size;
        WGPUBuffer unpack_src;  /* Storage|CopyDst: header + Z24S8 input */
        size_t unpack_src_size;
        /* GPU-sourced unpack: stencil bytes extracted on the GPU */
        WGPUBindGroupLayout stencil_bgl;
        WGPUComputePipeline stencil_pipeline;
        WGPUBuffer stencil_dst; /* Storage|CopySrc: padded stencil rows */
        size_t stencil_dst_size;
        WGPUBuffer xfer;        /* CopySrc|CopyDst: color->zeta bytes */
        size_t xfer_size;
    } compute;
} PGRAPHWgpuSurfaceState;

/* Owned guest range, not necessarily the current render image's footprint. */
static inline size_t pgraph_wgpu_surface_memory_size(const SurfaceBinding *s)
{
    return s->backing ? s->backing->size : s->size;
}

/* pgraph.lock held: order the current render image into its private backing. */
void pgraph_wgpu_merge_surface_backing(PGRAPHState *pg,
                                      SurfaceBinding *surface);

/* pgraph.lock held: materialize retained ranges before raw VRAM access. */
void pgraph_wgpu_materialize_retained(NV2AState *d, hwaddr addr, hwaddr size,
                                     bool write);
/* PFIFO-only entry; takes pgraph.lock only when there are retained ranges. */
void pgraph_wgpu_pre_read_command(NV2AState *d, hwaddr addr, hwaddr size);

/* Native linear byte reshaping; destination is distinct, tight BGRA8. */
bool pgraph_wgpu_linear_reshape_enabled(void);
bool pgraph_wgpu_is_linear_bgra(const SurfaceBinding *s);
void pgraph_wgpu_reshape_surface(PGRAPHState *pg, const SurfaceBinding *src,
                                 WGPUTextureView dst, hwaddr dst_addr,
                                 unsigned int width, unsigned int height,
                                 bool opaque);
void pgraph_wgpu_finalize_surface_reshape(PGRAPHState *pg);

/* surface module internal (surface-compute.c), used by surface.c */
void pgraph_wgpu_init_surface_compute(PGRAPHState *pg);
void pgraph_wgpu_finalize_surface_compute(PGRAPHState *pg);
/* Record into enc: pack surface depth+stencil into Z24S8 words in
 * r->surf.compute.pack_dst (width*height*4 bytes, tightly packed). */
void pgraph_wgpu_pack_depth_stencil(PGRAPHState *pg, SurfaceBinding *surface,
                                    WGPUCommandEncoder enc);
/* Upload guest Z24S8 data (width*height u32, tightly packed) into the
 * surface: stencil via a queue write, depth via a render pass recorded into
 * enc. Caller must have finished (no pending commands reading the surface
 * that must precede the queue writes). */
void pgraph_wgpu_unpack_depth_stencil_gpu(PGRAPHState *pg,
                                          SurfaceBinding *surface,
                                          WGPUCommandEncoder enc,
                                          WGPUBuffer src, size_t src_offset);
void pgraph_wgpu_zeta_from_color(PGRAPHState *pg, SurfaceBinding *zeta,
                                 SurfaceBinding *color,
                                 WGPUCommandEncoder enc, const uint8_t *tail);
void pgraph_wgpu_unpack_depth_stencil(PGRAPHState *pg, SurfaceBinding *surface,
                                      WGPUCommandEncoder enc,
                                      const uint32_t *z24s8);

/* re-enable CPU access trapping for a surface whose GPU copy is current */
void pgraph_wgpu_surface_rearm_cpu_trap(NV2AState *d, SurfaceBinding *surface);

#endif
