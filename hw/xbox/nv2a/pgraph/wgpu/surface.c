/*
 * Geforce NV2A PGRAPH WebGPU Renderer: surfaces (render targets)
 *
 * Port of the Vulkan renderer's surface.c (../vk/surface.c).
 *
 * Copyright (c) 2024-2025 Matt Borgerson
 *
 * Based on GL implementation:
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2015 Jannik Vogel
 * Copyright (c) 2018-2024 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "hw/xbox/nv2a/nv2a_int.h"
#include "hw/xbox/nv2a/pgraph/swizzle.h"
#include "qemu/compiler.h"
#include "ui/xemu-settings.h"
#include "renderer.h"
#include "qemu/xemu-wasm-stats.h"

static const int num_invalid_surfaces_to_keep = 10;  // FIXME: Make automatic
static const int max_surface_frame_time_delta = 5;

static const BasicSurfaceFormatInfo kelvin_surface_color_format_map[] = {
    [NV097_SET_SURFACE_FORMAT_COLOR_LE_X1R5G5B5_Z1R5G5B5] = { 2 },
    [NV097_SET_SURFACE_FORMAT_COLOR_LE_R5G6B5] = { 2 },
    [NV097_SET_SURFACE_FORMAT_COLOR_LE_X8R8G8B8_Z8R8G8B8] = { 4 },
    [NV097_SET_SURFACE_FORMAT_COLOR_LE_A8R8G8B8] = { 4 },
    [NV097_SET_SURFACE_FORMAT_COLOR_LE_B8] = { 1 },
    [NV097_SET_SURFACE_FORMAT_COLOR_LE_G8B8] = { 2 },
};

static const WgpuSurfaceFormatInfo kelvin_surface_color_format_wgpu_map[] = {
    [NV097_SET_SURFACE_FORMAT_COLOR_LE_X1R5G5B5_Z1R5G5B5] =
        // FIXME: Force alpha to zero (vk renders into A1R5G5B5 as well)
        { WGPUTextureFormat_BGRA8Unorm, 4, false, false,
          WGPU_SURFACE_CONV_X1R5G5B5 },
    [NV097_SET_SURFACE_FORMAT_COLOR_LE_R5G6B5] =
        { WGPUTextureFormat_BGRA8Unorm, 4, false, false,
          WGPU_SURFACE_CONV_R5G6B5 },
    [NV097_SET_SURFACE_FORMAT_COLOR_LE_X8R8G8B8_Z8R8G8B8] =
        // FIXME: Force alpha to zero
        { WGPUTextureFormat_BGRA8Unorm, 4, false, false,
          WGPU_SURFACE_CONV_NONE },
    [NV097_SET_SURFACE_FORMAT_COLOR_LE_A8R8G8B8] =
        { WGPUTextureFormat_BGRA8Unorm, 4, false, false,
          WGPU_SURFACE_CONV_NONE },
    [NV097_SET_SURFACE_FORMAT_COLOR_LE_B8] =
        // FIXME: Map channel color
        { WGPUTextureFormat_R8Unorm, 1, false, false, WGPU_SURFACE_CONV_NONE },
    [NV097_SET_SURFACE_FORMAT_COLOR_LE_G8B8] =
        // FIXME: Map channel color
        { WGPUTextureFormat_RG8Unorm, 2, false, false,
          WGPU_SURFACE_CONV_NONE },
};

static const BasicSurfaceFormatInfo kelvin_surface_zeta_format_map[] = {
    [NV097_SET_SURFACE_FORMAT_ZETA_Z16] = { 2 },
    [NV097_SET_SURFACE_FORMAT_ZETA_Z24S8] = { 4 },
};

// FIXME: Actually support stored float format

static const WgpuSurfaceFormatInfo zeta_d16 = {
    WGPUTextureFormat_Depth16Unorm, 2, true, false, WGPU_SURFACE_CONV_NONE,
};

static const WgpuSurfaceFormatInfo zeta_d32_float_s8 = {
    WGPUTextureFormat_Depth32FloatStencil8, 8, true, true,
    WGPU_SURFACE_CONV_Z24S8,
};

static const WgpuSurfaceFormatInfo zeta_d24_plus_s8 = {
    WGPUTextureFormat_Depth24PlusStencil8, 4, true, true,
    WGPU_SURFACE_CONV_Z24S8,
};

void pgraph_wgpu_set_surface_scale_factor(NV2AState *d, unsigned int scale)
{
    PGRAPHWgpuState *r = d->pgraph.wgpu_renderer_state;

    g_config.display.quality.surface_scale = scale < 1 ? 1 : scale;

    qemu_mutex_lock(&d->pfifo.lock);
    qatomic_set(&d->pfifo.halt, true);
    qemu_mutex_unlock(&d->pfifo.lock);

    // FIXME: It's just flush
    qemu_mutex_lock(&d->pgraph.lock);
    qemu_event_reset(&r->surf.dirty_surfaces_download_complete);
    qatomic_set(&r->surf.download_dirty_surfaces_pending, true);
    qemu_mutex_unlock(&d->pgraph.lock);
    qemu_mutex_lock(&d->pfifo.lock);
    pfifo_kick(d);
    qemu_mutex_unlock(&d->pfifo.lock);
    qemu_event_wait(&r->surf.dirty_surfaces_download_complete);

    qemu_mutex_lock(&d->pgraph.lock);
    qemu_event_reset(&d->pgraph.flush_complete);
    qatomic_set(&d->pgraph.flush_pending, true);
    qemu_mutex_unlock(&d->pgraph.lock);
    qemu_mutex_lock(&d->pfifo.lock);
    pfifo_kick(d);
    qemu_mutex_unlock(&d->pfifo.lock);
    qemu_event_wait(&d->pgraph.flush_complete);

    qemu_mutex_lock(&d->pfifo.lock);
    qatomic_set(&d->pfifo.halt, false);
    pfifo_kick(d);
    qemu_mutex_unlock(&d->pfifo.lock);
}

unsigned int pgraph_wgpu_get_surface_scale_factor(NV2AState *d)
{
    return d->pgraph.surface_scale_factor; // FIXME: Move internal to renderer
}

void pgraph_wgpu_reload_surface_scale_factor(PGRAPHState *pg)
{
    /*
     * FIXME: Surface scaling is not implemented in the WebGPU renderer yet;
     * the configured value is kept (see set_surface_scale_factor) but
     * surfaces are always rendered at native resolution.
     */
    pg->surface_scale_factor = 1;
}

// FIXME: Move to common
static void get_surface_dimensions(PGRAPHState const *pg, unsigned int *width,
                                   unsigned int *height)
{
    bool swizzle = (pg->surface_type == NV097_SET_SURFACE_FORMAT_TYPE_SWIZZLE);
    if (swizzle) {
        *width = 1 << pg->surface_shape.log_width;
        *height = 1 << pg->surface_shape.log_height;
    } else {
        *width = pg->surface_shape.clip_width;
        *height = pg->surface_shape.clip_height;
    }
}

// FIXME: Move to common
static bool framebuffer_dirty(PGRAPHState const *pg)
{
    bool shape_changed = memcmp(&pg->surface_shape, &pg->last_surface_shape,
                                sizeof(SurfaceShape)) != 0;
    if (!shape_changed || (!pg->surface_shape.color_format
            && !pg->surface_shape.zeta_format)) {
        return false;
    }
    return true;
}

/* Copy `height` rows of `row_bytes` bytes between strided images. */
static void copy_rows(uint8_t *dst, size_t dst_stride, const uint8_t *src,
                      size_t src_stride, size_t row_bytes, unsigned int height)
{
    if (dst_stride == src_stride && row_bytes == src_stride) {
        memcpy(dst, src, row_bytes * height);
        return;
    }
    for (unsigned int i = 0; i < height; i++) {
        memcpy(dst, src, row_bytes);
        dst += dst_stride;
        src += src_stride;
    }
}

/*
 * 16-bit color <-> BGRA8 (little-endian 0xAARRGGBB) conversion for the
 * formats WebGPU cannot render to directly.
 */
static inline uint32_t expand5(uint32_t v)
{
    return (v << 3) | (v >> 2);
}

static inline uint32_t expand6(uint32_t v)
{
    return (v << 2) | (v >> 4);
}

static inline uint32_t unorm8_to_n(uint32_t v, uint32_t max)
{
    return (v * max + 127) / 255;
}

static void convert_16bpp_to_host(WgpuSurfaceConversion conv, uint32_t *dst,
                                  size_t dst_stride_pixels, const uint8_t *src,
                                  size_t src_stride, unsigned int width,
                                  unsigned int height)
{
    for (unsigned int y = 0; y < height; y++) {
        const uint8_t *s = src + y * src_stride;
        uint32_t *o = dst + (size_t)y * dst_stride_pixels;
        for (unsigned int x = 0; x < width; x++) {
            uint32_t p = s[2 * x] | (s[2 * x + 1] << 8);
            uint32_t r, g, b, a;
            if (conv == WGPU_SURFACE_CONV_R5G6B5) {
                r = expand5((p >> 11) & 0x1f);
                g = expand6((p >> 5) & 0x3f);
                b = expand5(p & 0x1f);
                a = 0xff;
            } else {
                a = (p & 0x8000) ? 0xff : 0;
                r = expand5((p >> 10) & 0x1f);
                g = expand5((p >> 5) & 0x1f);
                b = expand5(p & 0x1f);
            }
            o[x] = (a << 24) | (r << 16) | (g << 8) | b;
        }
    }
}

static void convert_host_to_16bpp(WgpuSurfaceConversion conv, uint8_t *dst,
                                  size_t dst_stride, const uint8_t *src,
                                  size_t src_stride, unsigned int width,
                                  unsigned int height)
{
    for (unsigned int y = 0; y < height; y++) {
        const uint8_t *s = src + y * src_stride;
        uint8_t *o = dst + y * dst_stride;
        for (unsigned int x = 0; x < width; x++) {
            uint32_t b = s[4 * x], g = s[4 * x + 1], r = s[4 * x + 2],
                     a = s[4 * x + 3];
            uint32_t p;
            if (conv == WGPU_SURFACE_CONV_R5G6B5) {
                p = (unorm8_to_n(r, 31) << 11) | (unorm8_to_n(g, 63) << 5) |
                    unorm8_to_n(b, 31);
            } else {
                p = ((a >= 128) ? 0x8000 : 0) | (unorm8_to_n(r, 31) << 10) |
                    (unorm8_to_n(g, 31) << 5) | unorm8_to_n(b, 31);
            }
            o[2 * x] = p & 0xff;
            o[2 * x + 1] = p >> 8;
        }
    }
}

static WGPUBuffer ensure_staging_dst(PGRAPHWgpuState *r, size_t size)
{
    if (r->surf.staging_dst && r->surf.staging_dst_size >= size) {
        return r->surf.staging_dst;
    }
    if (r->surf.staging_dst) {
        wgpuBufferRelease(r->surf.staging_dst);
    }
    size = ROUND_UP(size, 64 * 1024);
    r->surf.staging_dst = wgpuDeviceCreateBuffer(
        r->device, &(WGPUBufferDescriptor){
                       .label = { "surface download staging", WGPU_STRLEN },
                       .usage = WGPUBufferUsage_CopyDst |
                                WGPUBufferUsage_MapRead,
                       .size = size });
    r->surf.staging_dst_size = size;
    return r->surf.staging_dst;
}

static bool check_surface_overlaps_range(const SurfaceBinding *surface,
                                         hwaddr range_start, hwaddr range_len)
{
    hwaddr surface_end = surface->vram_addr + surface->size;
    hwaddr range_end = range_start + range_len;
    return !(surface->vram_addr >= range_end || range_start >= surface_end);
}

void pgraph_wgpu_download_surfaces_in_range_if_dirty(PGRAPHState *pg,
                                                     hwaddr start, hwaddr size)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;
    SurfaceBinding *surface;

    QTAILQ_FOREACH(surface, &r->surf.surfaces, entry) {
        if (check_surface_overlaps_range(surface, start, size)) {
            pgraph_wgpu_surface_download_if_dirty(
                container_of(pg, NV2AState, pgraph), surface);
        }
    }
}

static void download_surface_to_buffer(NV2AState *d, SurfaceBinding *surface,
                                       uint8_t *pixels)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;

    if (!surface->width || !surface->height) {
        return;
    }

    nv2a_profile_inc_counter(NV2A_PROF_SURF_DOWNLOAD);

    bool use_compute_to_convert_depth_stencil_format =
        surface->host_fmt.conv == WGPU_SURFACE_CONV_Z24S8;

    trace_nv2a_pgraph_surface_download(
        surface->color ? "COLOR" : "ZETA",
        surface->swizzle ? "sz" : "lin", surface->vram_addr,
        surface->width, surface->height, surface->pitch,
        surface->fmt.bytes_per_pixel);

    // Read surface into memory
    uint8_t *gl_read_buf = pixels;

    uint8_t *swizzle_buf = pixels;
    if (surface->swizzle) {
        // FIXME: Swizzle in shader
        swizzle_buf = (uint8_t *)g_malloc(surface->size);
        gl_read_buf = swizzle_buf;
    }

    unsigned int width = surface->width, height = surface->height;

    /*
     * Record the copy after any pending draws into the open encoder, then
     * submit everything and wait (pgraph_wgpu_finish) before mapping.
     */
    WGPUCommandEncoder enc = pgraph_wgpu_begin_nondraw_commands(pg);
    WGPUBuffer staging;
    size_t staging_stride;

    if (use_compute_to_convert_depth_stencil_format) {
        size_t packed_size = (size_t)width * height * 4;
        staging_stride = width * 4;
        pgraph_wgpu_pack_depth_stencil(pg, surface, enc);
        staging = ensure_staging_dst(r, packed_size);
        wgpuCommandEncoderCopyBufferToBuffer(enc, r->surf.compute.pack_dst, 0,
                                             staging, 0, packed_size);
    } else {
        staging_stride =
            ROUND_UP(width * surface->host_fmt.host_bytes_per_pixel, 256);
        staging = ensure_staging_dst(r, staging_stride * height);
        WGPUTexelCopyTextureInfo src = {
            .texture = surface->texture,
            .aspect = surface->color ? WGPUTextureAspect_All :
                                       WGPUTextureAspect_DepthOnly,
        };
        WGPUTexelCopyBufferInfo dst = {
            .layout = { .offset = 0,
                        .bytesPerRow = staging_stride,
                        .rowsPerImage = height },
            .buffer = staging,
        };
        wgpuCommandEncoderCopyTextureToBuffer(
            enc, &src, &dst, &(WGPUExtent3D){ width, height, 1 });
    }
    pgraph_wgpu_end_nondraw_commands(pg, enc);

    nv2a_profile_inc_counter(NV2A_PROF_QUEUE_SUBMIT_1);
    pgraph_wgpu_finish(pg, WGPU_FINISH_REASON_SURFACE_DOWN);

    size_t read_size = staging_stride * height;
    g_autofree uint8_t *host = g_malloc(read_size);
    XSTAT_INC(n_surf_download);
    XSTAT_ADD(b_surf_download, read_size);
    pgraph_wgpu_read_buffer_sync(r, staging, 0, read_size, host);

    size_t row_bytes = width * surface->fmt.bytes_per_pixel;
    if (surface->host_fmt.conv == WGPU_SURFACE_CONV_R5G6B5 ||
        surface->host_fmt.conv == WGPU_SURFACE_CONV_X1R5G5B5) {
        unsigned int w = MIN(width, surface->pitch / 2);
        convert_host_to_16bpp(surface->host_fmt.conv, gl_read_buf,
                              surface->pitch, host, staging_stride, w, height);
    } else {
        copy_rows(gl_read_buf, surface->pitch, host, staging_stride,
                  MIN(row_bytes, surface->pitch), height);
    }

    if (surface->swizzle) {
        // FIXME: Swizzle in shader
        swizzle_rect(swizzle_buf, surface->width, surface->height, pixels,
                     surface->pitch, surface->fmt.bytes_per_pixel);
        nv2a_profile_inc_counter(NV2A_PROF_SURF_SWIZZLE);
        g_free(swizzle_buf);
    }
}

const char *pgraph_wgpu_dl_reason;

static void download_surface(NV2AState *d, SurfaceBinding *surface, bool force)
{
    if (!(surface->download_pending || force) || !surface->width ||
        !surface->height) {
        return;
    }
#ifdef EMSCRIPTEN
    {
        extern void xemu_wasm_count(const char *key);
        char key[128];
        snprintf(key, sizeof(key), "%s %s %ux%u fmt%u",
                 pgraph_wgpu_dl_reason ? pgraph_wgpu_dl_reason : "?",
                 surface->color ? "color" : "zeta", surface->width,
                 surface->height, surface->shape.color_format);
        xemu_wasm_count(g_intern_string(key));
    }
#endif

    // FIXME: Respect write enable at last TOU?

    download_surface_to_buffer(d, surface, d->vram_ptr + surface->vram_addr);

    memory_region_set_client_dirty(d->vram, surface->vram_addr,
                                   surface->pitch * surface->height,
                                   DIRTY_MEMORY_VGA);
    memory_region_set_client_dirty(d->vram, surface->vram_addr,
                                   surface->pitch * surface->height,
                                   DIRTY_MEMORY_NV2A_TEX);

    surface->download_pending = false;
    surface->draw_dirty = false;
}

void pgraph_wgpu_wait_for_surface_download(SurfaceBinding *surface)
{
    NV2AState *d = g_nv2a;
    PGRAPHWgpuState *r = d->pgraph.wgpu_renderer_state;

    if (qatomic_read(&surface->draw_dirty)) {
        qemu_mutex_lock(&d->pfifo.lock);
        qemu_event_reset(&r->surf.downloads_complete);
        qatomic_set(&surface->download_pending, true);
        qatomic_set(&r->surf.downloads_pending, true);
        pfifo_kick(d);
        qemu_mutex_unlock(&d->pfifo.lock);
        {
            /* guest CPU blocked until the GPU thread reads the surface back */
            XSTAT_T0();
            qemu_event_wait(&r->surf.downloads_complete);
            XSTAT_T1(ns_vcpu_surface_wait);
        }
    }
}

void pgraph_wgpu_process_pending_downloads(NV2AState *d)
{
    PGRAPHWgpuState *r = d->pgraph.wgpu_renderer_state;
    SurfaceBinding *surface;

    pgraph_wgpu_dl_reason = "cpu-access";
    QTAILQ_FOREACH(surface, &r->surf.surfaces, entry) {
        download_surface(d, surface, false);
    }

    qatomic_set(&r->surf.downloads_pending, false);
    qemu_event_set(&r->surf.downloads_complete);
}

void pgraph_wgpu_download_dirty_surfaces(NV2AState *d)
{
    PGRAPHWgpuState *r = d->pgraph.wgpu_renderer_state;

    SurfaceBinding *surface;
    pgraph_wgpu_dl_reason = "dirty-all";
    QTAILQ_FOREACH(surface, &r->surf.surfaces, entry) {
        pgraph_wgpu_surface_download_if_dirty(d, surface);
    }

    qatomic_set(&r->surf.download_dirty_surfaces_pending, false);
    qemu_event_set(&r->surf.dirty_surfaces_download_complete);
}

static void disarm_cpu_access_callback(NV2AState *d, SurfaceBinding *surface);

static void surface_access_callback(void *opaque, MemoryRegion *mr, hwaddr addr,
                                    hwaddr len, bool write)
{
    NV2AState *d = (NV2AState *)opaque;
    qemu_mutex_lock(&d->pgraph.lock);

    PGRAPHWgpuState *r = d->pgraph.wgpu_renderer_state;
    bool wait_for_downloads = false;

    SurfaceBinding *surface;
    QTAILQ_FOREACH(surface, &r->surf.surfaces, entry) {
        if (!check_surface_overlaps_range(surface, addr, len)) {
            continue;
        }

        hwaddr offset = addr - surface->vram_addr;

        if (write) {
            trace_nv2a_pgraph_surface_cpu_write(surface->vram_addr, offset);
        } else {
            trace_nv2a_pgraph_surface_cpu_read(surface->vram_addr, offset);
        }

        if (surface->draw_dirty) {
            surface->download_pending = true;
            wait_for_downloads = true;
        }

        if (write) {
            surface->upload_pending = true;
        }

        /*
         * One trap per GPU->CPU handover is enough: once any pending
         * download has completed (we wait below) VRAM is current, and a
         * write has marked the surface for re-upload. Keep trapping and
         * every further CPU access (millions/s when a game writes video
         * frames into a render surface) pays for this callback.
         * Re-armed by pgraph_wgpu_surface_rearm_cpu_trap() once the GPU
         * copy is current again (GPU draw or upload).
         */
        disarm_cpu_access_callback(d, surface);
    }

    qemu_mutex_unlock(&d->pgraph.lock);

    if (wait_for_downloads) {
        qemu_mutex_lock(&d->pfifo.lock);
        qemu_event_reset(&r->surf.downloads_complete);
        qatomic_set(&r->surf.downloads_pending, true);
        pfifo_kick(d);
        qemu_mutex_unlock(&d->pfifo.lock);
        {
            /* guest CPU blocked until the GPU thread reads the surface back */
            XSTAT_T0();
            qemu_event_wait(&r->surf.downloads_complete);
            XSTAT_T1(ns_vcpu_surface_wait);
        }
    }
}

static void register_cpu_access_callback(NV2AState *d, SurfaceBinding *surface)
{
    if (surface->access_cb) {
        return; /* already armed */
    }
    if (tcg_enabled()) {
        if (surface->width && surface->height) {
            surface->access_cb = mem_access_callback_insert(
                qemu_get_cpu(0), d->vram, surface->vram_addr, surface->size,
                &surface_access_callback, d);
        } else {
            surface->access_cb = NULL;
        }
    }
}

static void unregister_cpu_access_callback(NV2AState *d,
                                           SurfaceBinding *surface)
{
    if (tcg_enabled() && surface->access_cb) {
        mem_access_callback_remove_by_ref(qemu_get_cpu(0), surface->access_cb);
    }
    surface->access_cb = NULL;
}

/* pgraph.lock held */
static void disarm_cpu_access_callback(NV2AState *d, SurfaceBinding *surface)
{
    if (surface->access_cb) {
        XSTAT_INC(n_watch_disarm);
        unregister_cpu_access_callback(d, surface);
    }
}

/*
 * The GPU copy of @surface just became authoritative again (it was drawn to,
 * or VRAM was uploaded into it): trap the next CPU access. pgraph.lock held.
 */
void pgraph_wgpu_surface_rearm_cpu_trap(NV2AState *d, SurfaceBinding *surface)
{
    PGRAPHWgpuState *r = d->pgraph.wgpu_renderer_state;
    SurfaceBinding *s;

    /* only surfaces still in the live list are trapped */
    QTAILQ_FOREACH(s, &r->surf.surfaces, entry) {
        if (s == surface) {
            register_cpu_access_callback(d, surface);
            return;
        }
    }
}

static void bind_surface(PGRAPHWgpuState *r, SurfaceBinding *surface)
{
    if (surface->color) {
        r->color_binding = surface;
    } else {
        r->zeta_binding = surface;
    }

    r->framebuffer_dirty = true;
}

static void unbind_surface(NV2AState *d, bool color)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;

    if (color) {
        if (r->color_binding) {
            r->color_binding = NULL;
            r->framebuffer_dirty = true;
        }
    } else {
        if (r->zeta_binding) {
            r->zeta_binding = NULL;
            r->framebuffer_dirty = true;
        }
    }
}

static void invalidate_surface(NV2AState *d, SurfaceBinding *surface)
{
    PGRAPHWgpuState *r = d->pgraph.wgpu_renderer_state;

    trace_nv2a_pgraph_surface_invalidated(surface->vram_addr);

    // FIXME: We may be reading from the surface in the current command buffer!
    // Add a detection to handle it. For now, finish to be safe.
    pgraph_wgpu_finish(&d->pgraph, WGPU_FINISH_REASON_SURFACE_DOWN);

    if (surface == r->color_binding) {
        assert(d->pgraph.surface_color.buffer_dirty);
        unbind_surface(d, true);
    }
    if (surface == r->zeta_binding) {
        assert(d->pgraph.surface_zeta.buffer_dirty);
        unbind_surface(d, false);
    }

    unregister_cpu_access_callback(d, surface);

    QTAILQ_REMOVE(&r->surf.surfaces, surface, entry);
    QTAILQ_INSERT_HEAD(&r->surf.invalid_surfaces, surface, entry);
}

static bool check_surfaces_overlap(const SurfaceBinding *surface,
                                   const SurfaceBinding *other_surface)
{
    return check_surface_overlaps_range(surface, other_surface->vram_addr,
                                        other_surface->size);
}

static void invalidate_overlapping_surfaces(NV2AState *d,
                                            SurfaceBinding const *surface)
{
    PGRAPHWgpuState *r = d->pgraph.wgpu_renderer_state;

    SurfaceBinding *other_surface, *next_surface;
    QTAILQ_FOREACH_SAFE (other_surface, &r->surf.surfaces, entry,
                         next_surface) {
        if (check_surfaces_overlap(surface, other_surface)) {
            trace_nv2a_pgraph_surface_evict_overlapping(
                other_surface->vram_addr, other_surface->width,
                other_surface->height, other_surface->pitch);
            pgraph_wgpu_dl_reason = "overlap-evict";
            pgraph_wgpu_surface_download_if_dirty(d, other_surface);
            invalidate_surface(d, other_surface);
        }
    }
}

static void surface_put(NV2AState *d, SurfaceBinding *surface)
{
    PGRAPHWgpuState *r = d->pgraph.wgpu_renderer_state;

    assert(pgraph_wgpu_surface_get(d, surface->vram_addr) == NULL);

    invalidate_overlapping_surfaces(d, surface);
    register_cpu_access_callback(d, surface);

    QTAILQ_INSERT_HEAD(&r->surf.surfaces, surface, entry);
}

SurfaceBinding *pgraph_wgpu_surface_get(NV2AState *d, hwaddr addr)
{
    PGRAPHWgpuState *r = d->pgraph.wgpu_renderer_state;

    SurfaceBinding *surface;
    QTAILQ_FOREACH (surface, &r->surf.surfaces, entry) {
        if (surface->vram_addr == addr) {
            return surface;
        }
    }

    return NULL;
}

SurfaceBinding *pgraph_wgpu_surface_get_within(NV2AState *d, hwaddr addr)
{
    PGRAPHWgpuState *r = d->pgraph.wgpu_renderer_state;

    SurfaceBinding *surface;
    QTAILQ_FOREACH (surface, &r->surf.surfaces, entry) {
        if (addr >= surface->vram_addr &&
            addr < (surface->vram_addr + surface->size)) {
            return surface;
        }
    }

    return NULL;
}

static void set_surface_label(PGRAPHState *pg, SurfaceBinding const *surface)
{
    g_autofree gchar *label = g_strdup_printf(
        "Surface %" HWADDR_PRIx "h fmt:%s,%02xh %dx%d aa:%d",
        surface->vram_addr, surface->color ? "Color" : "Zeta",
        surface->color ? surface->shape.color_format :
                         surface->shape.zeta_format,
        surface->width, surface->height, pg->surface_shape.anti_aliasing);

    wgpuTextureSetLabel(surface->texture, (WGPUStringView){ label,
                                                            WGPU_STRLEN });
}

static void create_surface_image(PGRAPHState *pg, SurfaceBinding *surface)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;

    unsigned int width = surface->width ? surface->width : 1;
    unsigned int height = surface->height ? surface->height : 1;
    pgraph_apply_scaling_factor(pg, &width, &height);

    assert(!surface->texture);

    NV2A_DPRINTF("Creating new surface texture width=%d height=%d @ %08"
                 HWADDR_PRIx "\n", width, height, surface->vram_addr);

    surface->texture = wgpuDeviceCreateTexture(
        r->device, &(WGPUTextureDescriptor){
                       .label = { "surface", WGPU_STRLEN },
                       .usage = WGPUTextureUsage_RenderAttachment |
                                WGPUTextureUsage_TextureBinding |
                                WGPUTextureUsage_CopySrc |
                                WGPUTextureUsage_CopyDst,
                       .dimension = WGPUTextureDimension_2D,
                       .size = { width, height, 1 },
                       .format = surface->host_fmt.format,
                       .mipLevelCount = 1,
                       .sampleCount = 1 });
    surface->view = wgpuTextureCreateView(surface->texture, NULL);

    if (surface->host_fmt.depth) {
        WGPUTextureViewDescriptor vd = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
        vd.label = (WGPUStringView){ "surface depth", WGPU_STRLEN };
        vd.dimension = WGPUTextureViewDimension_2D;
        vd.aspect = WGPUTextureAspect_DepthOnly;
        surface->depth_view = wgpuTextureCreateView(surface->texture, &vd);
    }
    if (surface->host_fmt.stencil) {
        WGPUTextureViewDescriptor vd = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
        vd.label = (WGPUStringView){ "surface stencil", WGPU_STRLEN };
        vd.dimension = WGPUTextureViewDimension_2D;
        vd.aspect = WGPUTextureAspect_StencilOnly;
        surface->stencil_view = wgpuTextureCreateView(surface->texture, &vd);
    }

    nv2a_profile_inc_counter(NV2A_PROF_SURF_CREATE);
}

static void migrate_surface_image(SurfaceBinding *dst, SurfaceBinding *src)
{
    dst->texture = src->texture;
    dst->view = src->view;
    dst->depth_view = src->depth_view;
    dst->stencil_view = src->stencil_view;

    src->texture = NULL;
    src->view = NULL;
    src->depth_view = NULL;
    src->stencil_view = NULL;
}

static void destroy_surface_image(PGRAPHWgpuState *r, SurfaceBinding *surface)
{
    /*
     * Release only (no wgpuTextureDestroy): anything still referencing the
     * texture (e.g. a texture-module bind group) keeps it alive.
     */
    if (surface->stencil_view) {
        wgpuTextureViewRelease(surface->stencil_view);
        surface->stencil_view = NULL;
    }
    if (surface->depth_view) {
        wgpuTextureViewRelease(surface->depth_view);
        surface->depth_view = NULL;
    }
    if (surface->view) {
        wgpuTextureViewRelease(surface->view);
        surface->view = NULL;
    }
    if (surface->texture) {
        wgpuTextureRelease(surface->texture);
        surface->texture = NULL;
    }
}

static bool check_invalid_surface_is_compatibile(SurfaceBinding *surface,
                                                 SurfaceBinding *target)
{
    return surface->host_fmt.format == target->host_fmt.format &&
           surface->width == target->width &&
           surface->height == target->height;
}

static SurfaceBinding *
get_any_compatible_invalid_surface(PGRAPHWgpuState *r, SurfaceBinding *target)
{
    SurfaceBinding *surface, *next;
    QTAILQ_FOREACH_SAFE(surface, &r->surf.invalid_surfaces, entry, next) {
        if (check_invalid_surface_is_compatibile(surface, target)) {
            QTAILQ_REMOVE(&r->surf.invalid_surfaces, surface, entry);
            return surface;
        }
    }

    return NULL;
}

static void prune_invalid_surfaces(PGRAPHWgpuState *r, int keep)
{
    int num_surfaces = 0;

    SurfaceBinding *surface, *next;
    QTAILQ_FOREACH_SAFE(surface, &r->surf.invalid_surfaces, entry, next) {
        num_surfaces += 1;
        if (num_surfaces > keep) {
            QTAILQ_REMOVE(&r->surf.invalid_surfaces, surface, entry);
            destroy_surface_image(r, surface);
            g_free(surface);
        }
    }
}

static void expire_old_surfaces(NV2AState *d)
{
    PGRAPHWgpuState *r = d->pgraph.wgpu_renderer_state;

    SurfaceBinding *s, *next;
    QTAILQ_FOREACH_SAFE(s, &r->surf.surfaces, entry, next) {
        int last_used = d->pgraph.frame_time - s->frame_time;
        if (last_used >= max_surface_frame_time_delta) {
            trace_nv2a_pgraph_surface_evict_reason("old", s->vram_addr);
            pgraph_wgpu_dl_reason = "expire-old";
            pgraph_wgpu_surface_download_if_dirty(d, s);
            invalidate_surface(d, s);
        }
    }
}

static bool check_surface_compatibility(SurfaceBinding const *s1,
                                        SurfaceBinding const *s2, bool strict)
{
    /*
     * Several guest formats share a host format here (e.g. R5G6B5 and
     * A8R8G8B8 both render into BGRA8Unorm), so also compare the guest
     * conversion and bytes per pixel to stay equivalent to vk's format check.
     */
    bool format_compatible =
        (s1->color == s2->color) &&
        (s1->host_fmt.format == s2->host_fmt.format) &&
        (s1->host_fmt.conv == s2->host_fmt.conv) &&
        (s1->fmt.bytes_per_pixel == s2->fmt.bytes_per_pixel) &&
        (s1->pitch == s2->pitch);
    if (!format_compatible) {
        return false;
    }

    if (!strict) {
        return (s1->width >= s2->width) && (s1->height >= s2->height);
    } else {
        return (s1->width == s2->width) && (s1->height == s2->height);
    }
}

void pgraph_wgpu_surface_download_if_dirty(NV2AState *d,
                                           SurfaceBinding *surface)
{
    if (surface->draw_dirty) {
        download_surface(d, surface, true);
    }
}

void pgraph_wgpu_upload_surface_data(NV2AState *d, SurfaceBinding *surface,
                                     bool force)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;

    if (!(surface->upload_pending || force)) {
        return;
    }

    nv2a_profile_inc_counter(NV2A_PROF_SURF_UPLOAD);

    /*
     * Queue writes are ordered before the next submit: submit everything
     * recorded so far first so the upload lands after it.
     */
    pgraph_wgpu_finish(pg, WGPU_FINISH_REASON_SURFACE_CREATE); // FIXME: SURFACE_UP

    trace_nv2a_pgraph_surface_upload(
                 surface->color ? "COLOR" : "ZETA",
                 surface->swizzle ? "sz" : "lin", surface->vram_addr,
                 surface->width, surface->height, surface->pitch,
                 surface->fmt.bytes_per_pixel);

    surface->upload_pending = false;
    surface->draw_time = pg->draw_time;

    if (!surface->width || !surface->height) {
        surface->initialized = true;
        return;
    }

    uint8_t *data = d->vram_ptr;
    uint8_t *buf = data + surface->vram_addr;

    g_autofree uint8_t *swizzle_buf = NULL;
    uint8_t *gl_read_buf = NULL;

    if (surface->swizzle) {
        swizzle_buf = (uint8_t*)g_malloc(surface->size);
        gl_read_buf = swizzle_buf;
        unswizzle_rect(data + surface->vram_addr,
                       surface->width, surface->height,
                       swizzle_buf,
                       surface->pitch,
                       surface->fmt.bytes_per_pixel);
        nv2a_profile_inc_counter(NV2A_PROF_SURF_SWIZZLE);
    } else {
        gl_read_buf = buf;
    }

    unsigned int width = surface->width, height = surface->height;
    size_t guest_row_bytes = width * surface->fmt.bytes_per_pixel;
    size_t src_row_bytes = MIN(guest_row_bytes, surface->pitch);

    if (surface->host_fmt.conv == WGPU_SURFACE_CONV_Z24S8) {
        /* Tightly packed Z24S8 words, unpacked on the GPU */
        g_autofree uint32_t *packed = g_malloc0((size_t)width * height * 4);
        copy_rows((uint8_t *)packed, guest_row_bytes, gl_read_buf,
                  surface->pitch, src_row_bytes, height);

        WGPUCommandEncoder enc = pgraph_wgpu_begin_nondraw_commands(pg);
        pgraph_wgpu_unpack_depth_stencil(pg, surface, enc, packed);
        pgraph_wgpu_end_nondraw_commands(pg, enc);
    } else {
        const uint8_t *upload_data;
        size_t upload_stride;
        g_autofree uint8_t *conv_buf = NULL;

        if (surface->host_fmt.conv == WGPU_SURFACE_CONV_R5G6B5 ||
            surface->host_fmt.conv == WGPU_SURFACE_CONV_X1R5G5B5) {
            conv_buf = g_malloc0((size_t)width * height * 4);
            /* rows narrower than the surface (pitch < width) stay zero */
            convert_16bpp_to_host(surface->host_fmt.conv, (uint32_t *)conv_buf,
                                  width, gl_read_buf, surface->pitch,
                                  MIN(width, surface->pitch / 2), height);
            upload_data = conv_buf;
            upload_stride = (size_t)width * 4;
        } else if (surface->pitch >= guest_row_bytes) {
            upload_data = gl_read_buf;
            upload_stride = surface->pitch;
        } else {
            conv_buf = g_malloc0(guest_row_bytes * height);
            copy_rows(conv_buf, guest_row_bytes, gl_read_buf, surface->pitch,
                      src_row_bytes, height);
            upload_data = conv_buf;
            upload_stride = guest_row_bytes;
        }

        size_t host_row_bytes =
            (size_t)width * surface->host_fmt.host_bytes_per_pixel;
        WGPUTexelCopyTextureInfo dst = {
            .texture = surface->texture,
            .aspect = surface->color ? WGPUTextureAspect_All :
                                       WGPUTextureAspect_DepthOnly,
        };
        WGPUTexelCopyBufferLayout layout = {
            .bytesPerRow = upload_stride,
            .rowsPerImage = height,
        };
        XSTAT_INC(n_surf_upload);
        XSTAT_ADD(b_surf_upload, upload_stride * (height - 1) + host_row_bytes);
        wgpuQueueWriteTexture(r->queue, &dst, upload_data,
                              upload_stride * (height - 1) + host_row_bytes,
                              &layout, &(WGPUExtent3D){ width, height, 1 });
    }

    nv2a_profile_inc_counter(NV2A_PROF_QUEUE_SUBMIT_2);

    surface->initialized = true;
    /* GPU copy == VRAM again: trap the next CPU write */
    pgraph_wgpu_surface_rearm_cpu_trap(d, surface);
}

static void compare_surfaces(SurfaceBinding const *a, SurfaceBinding const *b)
{
    #define DO_CMP(fld) \
        if (a->fld != b->fld) \
            trace_nv2a_pgraph_surface_compare_mismatch( \
                #fld, (long int)a->fld, (long int)b->fld);
    DO_CMP(shape.clip_x)
    DO_CMP(shape.clip_width)
    DO_CMP(shape.clip_y)
    DO_CMP(shape.clip_height)
    DO_CMP(fmt.bytes_per_pixel)
    DO_CMP(host_fmt.format)
    DO_CMP(color)
    DO_CMP(swizzle)
    DO_CMP(vram_addr)
    DO_CMP(width)
    DO_CMP(height)
    DO_CMP(pitch)
    DO_CMP(size)
    DO_CMP(dma_addr)
    DO_CMP(dma_len)
    DO_CMP(frame_time)
    DO_CMP(draw_time)
    #undef DO_CMP
}

static void populate_surface_binding_target_sized(NV2AState *d, bool color,
                                                  unsigned int width,
                                                  unsigned int height,
                                                  SurfaceBinding *target)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;

    Surface *surface;
    hwaddr dma_address;
    BasicSurfaceFormatInfo fmt;
    WgpuSurfaceFormatInfo host_fmt;

    if (color) {
        surface = &pg->surface_color;
        dma_address = pg->dma_color;
        assert(pg->surface_shape.color_format != 0);
        assert(pg->surface_shape.color_format <
               ARRAY_SIZE(kelvin_surface_color_format_wgpu_map));
        fmt = kelvin_surface_color_format_map[pg->surface_shape.color_format];
        host_fmt =
            kelvin_surface_color_format_wgpu_map[pg->surface_shape.color_format];
        if (host_fmt.host_bytes_per_pixel == 0) {
            fprintf(stderr, "nv2a: unimplemented color surface format 0x%x\n",
                    pg->surface_shape.color_format);
            abort();
        }
    } else {
        surface = &pg->surface_zeta;
        dma_address = pg->dma_zeta;
        assert(pg->surface_shape.zeta_format != 0);
        assert(pg->surface_shape.zeta_format <
               ARRAY_SIZE(r->surf.kelvin_surface_zeta_map));
        fmt = kelvin_surface_zeta_format_map[pg->surface_shape.zeta_format];
        host_fmt = r->surf.kelvin_surface_zeta_map[pg->surface_shape.zeta_format];
        // FIXME: Support float 16,24b float format surface
    }

    DMAObject dma = nv_dma_load(d, dma_address);
    // There's a bunch of bugs that could cause us to hit this function
    // at the wrong time and get a invalid dma object.
    // Check that it's sane.
    assert(dma.dma_class == NV_DMA_IN_MEMORY_CLASS);
    // assert(dma.address + surface->offset != 0);
    assert(surface->offset <= dma.limit);
    assert(surface->offset + surface->pitch * height <= dma.limit + 1);
    assert(surface->pitch % fmt.bytes_per_pixel == 0);
    assert((dma.address & ~0x07FFFFFF) == 0);

    target->shape = (color || !r->color_binding) ? pg->surface_shape :
                                                   r->color_binding->shape;
    target->fmt = fmt;
    target->host_fmt = host_fmt;
    target->color = color;
    target->swizzle =
        (pg->surface_type == NV097_SET_SURFACE_FORMAT_TYPE_SWIZZLE);
    target->vram_addr = dma.address + surface->offset;
    target->width = width;
    target->height = height;
    target->pitch = surface->pitch;
    target->size = height * MAX(surface->pitch, width * fmt.bytes_per_pixel);
    target->upload_pending = true;
    target->download_pending = false;
    target->draw_dirty = false;
    target->dma_addr = dma.address;
    target->dma_len = dma.limit;
    target->frame_time = pg->frame_time;
    target->draw_time = pg->draw_time;
    target->cleared = false;

    target->initialized = false;
}

static void populate_surface_binding_target(NV2AState *d, bool color,
                                            SurfaceBinding *target)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;

    unsigned int width, height;

    if (color || !r->color_binding) {
        get_surface_dimensions(pg, &width, &height);
        pgraph_apply_anti_aliasing_factor(pg, &width, &height);

        // Since we determine surface dimensions based on the clipping
        // rectangle, make sure to include the surface offset as well.
        if (pg->surface_type != NV097_SET_SURFACE_FORMAT_TYPE_SWIZZLE) {
            width += pg->surface_shape.clip_x;
            height += pg->surface_shape.clip_y;
        }
    } else {
        width = r->color_binding->width;
        height = r->color_binding->height;
    }

    populate_surface_binding_target_sized(d, color, width, height, target);
}

static void update_surface_part(NV2AState *d, bool upload, bool color)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;

    SurfaceBinding target;
    memset(&target, 0, sizeof(target));
    populate_surface_binding_target(d, color, &target);

    Surface *pg_surface = color ? &pg->surface_color : &pg->surface_zeta;

    bool mem_dirty = !tcg_enabled() && memory_region_test_and_clear_dirty(
                                           d->vram, target.vram_addr,
                                           target.size, DIRTY_MEMORY_NV2A);

    SurfaceBinding *current_binding = color ? r->color_binding
                                            : r->zeta_binding;

    if (!current_binding ||
        (upload && (pg_surface->buffer_dirty || mem_dirty))) {
        // FIXME: We don't need to be so aggressive flushing the command list
        // pgraph_wgpu_finish(pg, WGPU_FINISH_REASON_SURFACE_CREATE);
        pgraph_wgpu_ensure_not_in_render_pass(pg);

        unbind_surface(d, color);

        SurfaceBinding *surface = pgraph_wgpu_surface_get(d, target.vram_addr);
        if (surface != NULL) {
            // FIXME: Support same color/zeta surface target? In the mean time,
            // if the surface we just found is currently bound, just unbind it.
            SurfaceBinding *other = (color ? r->zeta_binding
                                           : r->color_binding);
            if (surface == other) {
                NV2A_UNIMPLEMENTED("Same color & zeta surface offset");
                unbind_surface(d, !color);
            }
        }

        trace_nv2a_pgraph_surface_target(
            color ? "COLOR" : "ZETA", target.vram_addr,
            target.swizzle ? "sz" : "ln",
            pg->surface_shape.anti_aliasing,
            pg->surface_shape.clip_x,
            pg->surface_shape.clip_width, pg->surface_shape.clip_y,
            pg->surface_shape.clip_height);

        bool should_create = true;

        if (surface != NULL) {
            bool is_compatible =
                check_surface_compatibility(surface, &target, false);

            void (*trace_fn)(uint32_t addr, uint32_t width, uint32_t height,
                             const char *layout, uint32_t anti_aliasing,
                             uint32_t clip_x, uint32_t clip_width,
                             uint32_t clip_y, uint32_t clip_height,
                             uint32_t pitch) =
                surface->color ? trace_nv2a_pgraph_surface_match_color :
                               trace_nv2a_pgraph_surface_match_zeta;

            trace_fn(surface->vram_addr, surface->width, surface->height,
                     surface->swizzle ? "sz" : "ln", surface->shape.anti_aliasing,
                     surface->shape.clip_x, surface->shape.clip_width,
                     surface->shape.clip_y, surface->shape.clip_height,
                     surface->pitch);

            assert(!(target.swizzle && pg->clearing));

            if (is_compatible && color &&
                !check_surface_compatibility(surface, &target, true)) {
                SurfaceBinding zeta_entry;
                populate_surface_binding_target_sized(
                    d, !color, surface->width, surface->height, &zeta_entry);
                hwaddr color_end = surface->vram_addr + surface->size;
                hwaddr zeta_end = zeta_entry.vram_addr + zeta_entry.size;
                is_compatible &= surface->vram_addr >= zeta_end ||
                                 zeta_entry.vram_addr >= color_end;
            }

            if (is_compatible && !color && r->color_binding) {
                is_compatible &= (surface->width == r->color_binding->width) &&
                                 (surface->height == r->color_binding->height);
            }

            if (is_compatible) {
                // FIXME: Refactor
                pg->surface_binding_dim.width = surface->width;
                pg->surface_binding_dim.clip_x = surface->shape.clip_x;
                pg->surface_binding_dim.clip_width = surface->shape.clip_width;
                pg->surface_binding_dim.height = surface->height;
                pg->surface_binding_dim.clip_y = surface->shape.clip_y;
                pg->surface_binding_dim.clip_height = surface->shape.clip_height;
                surface->upload_pending |= mem_dirty;
                pg->surface_zeta.buffer_dirty |= color;
                should_create = false;
            } else {
                trace_nv2a_pgraph_surface_evict_reason(
                    "incompatible", surface->vram_addr);
                compare_surfaces(surface, &target);
                pgraph_wgpu_dl_reason = "incompatible-rebind";
                pgraph_wgpu_surface_download_if_dirty(d, surface);
                invalidate_surface(d, surface);
            }
        }

        if (should_create) {
            surface = get_any_compatible_invalid_surface(r, &target);
            if (surface) {
                migrate_surface_image(&target, surface);
            } else {
                surface = g_malloc(sizeof(SurfaceBinding));
                create_surface_image(pg, &target);
            }

            *surface = target;
            set_surface_label(pg, surface);
            surface_put(d, surface);

            // FIXME: Refactor
            pg->surface_binding_dim.width = target.width;
            pg->surface_binding_dim.clip_x = target.shape.clip_x;
            pg->surface_binding_dim.clip_width = target.shape.clip_width;
            pg->surface_binding_dim.height = target.height;
            pg->surface_binding_dim.clip_y = target.shape.clip_y;
            pg->surface_binding_dim.clip_height = target.shape.clip_height;

            if (color && r->zeta_binding &&
                (r->zeta_binding->width != target.width ||
                 r->zeta_binding->height != target.height)) {
                pg->surface_zeta.buffer_dirty = true;
            }
        }

        void (*trace_fn)(uint32_t addr, uint32_t width, uint32_t height,
                         const char *layout, uint32_t anti_aliasing,
                         uint32_t clip_x, uint32_t clip_width, uint32_t clip_y,
                         uint32_t clip_height, uint32_t pitch) =
            color ? (should_create ? trace_nv2a_pgraph_surface_create_color :
                                     trace_nv2a_pgraph_surface_hit_color) :
                    (should_create ? trace_nv2a_pgraph_surface_create_zeta :
                                     trace_nv2a_pgraph_surface_hit_zeta);
        trace_fn(surface->vram_addr, surface->width, surface->height,
                 surface->swizzle ? "sz" : "ln", surface->shape.anti_aliasing,
                 surface->shape.clip_x, surface->shape.clip_width,
                 surface->shape.clip_y, surface->shape.clip_height, surface->pitch);

        bind_surface(r, surface);
        pg_surface->buffer_dirty = false;
    }

    if (!upload && pg_surface->draw_dirty) {
        if (!tcg_enabled()) {
            // FIXME: Cannot monitor for reads/writes; flush now
            download_surface(d, color ? r->color_binding : r->zeta_binding,
                             true);
        }

        pg_surface->write_enabled_cache = false;
        pg_surface->draw_dirty = false;
    }
}

// FIXME: Move to common?
void pgraph_wgpu_surface_update(NV2AState *d, bool upload, bool color_write,
                                bool zeta_write)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;

    pg->surface_shape.z_format =
        GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER),
                 NV_PGRAPH_SETUPRASTER_Z_FORMAT);

    color_write = color_write &&
            (pg->clearing || pgraph_color_write_enabled(pg));
    zeta_write = zeta_write && (pg->clearing || pgraph_zeta_write_enabled(pg));

    if (upload) {
        bool fb_dirty = framebuffer_dirty(pg);
        if (fb_dirty) {
            memcpy(&pg->last_surface_shape, &pg->surface_shape,
                   sizeof(SurfaceShape));
            pg->surface_color.buffer_dirty = true;
            pg->surface_zeta.buffer_dirty = true;
        }

        if (pg->surface_color.buffer_dirty) {
            unbind_surface(d, true);
        }

        if (color_write) {
            update_surface_part(d, true, true);
        }

        if (pg->surface_zeta.buffer_dirty) {
            unbind_surface(d, false);
        }

        if (zeta_write) {
            update_surface_part(d, true, false);
        }
    } else {
        if ((color_write || pg->surface_color.write_enabled_cache)
            && pg->surface_color.draw_dirty) {
            update_surface_part(d, false, true);
        }
        if ((zeta_write || pg->surface_zeta.write_enabled_cache)
            && pg->surface_zeta.draw_dirty) {
            update_surface_part(d, false, false);
        }
    }

    if (upload) {
        pg->draw_time++;
    }

    bool swizzle = (pg->surface_type == NV097_SET_SURFACE_FORMAT_TYPE_SWIZZLE);

    if (r->color_binding) {
        r->color_binding->frame_time = pg->frame_time;
        if (upload) {
            pgraph_wgpu_upload_surface_data(d, r->color_binding, false);
            r->color_binding->draw_time = pg->draw_time;
            r->color_binding->swizzle = swizzle;
        }
    }

    if (r->zeta_binding) {
        r->zeta_binding->frame_time = pg->frame_time;
        if (upload) {
            pgraph_wgpu_upload_surface_data(d, r->zeta_binding, false);
            r->zeta_binding->draw_time = pg->draw_time;
            r->zeta_binding->swizzle = swizzle;
        }
    }

    // Sanity check color and zeta dimensions match
    if (r->color_binding && r->zeta_binding) {
        assert(r->color_binding->width == r->zeta_binding->width);
        assert(r->color_binding->height == r->zeta_binding->height);
    }

    expire_old_surfaces(d);
    prune_invalid_surfaces(r, num_invalid_surfaces_to_keep);
}

void pgraph_wgpu_init_surfaces(PGRAPHState *pg)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;

    // WebGPU has no D24S8 format that is copyable/samplable per aspect in a
    // portable way; prefer Depth32FloatStencil8 (depth copies out as f32),
    // fall back to Depth24PlusStencil8. Both are packed/unpacked on the GPU.
    r->surf.kelvin_surface_zeta_map[NV097_SET_SURFACE_FORMAT_ZETA_Z16] =
        zeta_d16;
    r->surf.kelvin_surface_zeta_map[NV097_SET_SURFACE_FORMAT_ZETA_Z24S8] =
        r->has_depth32_stencil8 ? zeta_d32_float_s8 : zeta_d24_plus_s8;

    QTAILQ_INIT(&r->surf.surfaces);
    QTAILQ_INIT(&r->surf.invalid_surfaces);

    r->surf.downloads_pending = false;
    qemu_event_init(&r->surf.downloads_complete, false);
    qemu_event_init(&r->surf.dirty_surfaces_download_complete, false);

    r->color_binding = NULL;
    r->zeta_binding = NULL;
    r->framebuffer_dirty = true;

    pgraph_wgpu_init_surface_compute(pg);

    pgraph_wgpu_reload_surface_scale_factor(pg); // FIXME: Move internal
}

void pgraph_wgpu_finalize_surfaces(PGRAPHState *pg)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;

    pgraph_wgpu_surface_flush(container_of(pg, NV2AState, pgraph));

    pgraph_wgpu_finalize_surface_compute(pg);

    if (r->surf.staging_dst) {
        wgpuBufferRelease(r->surf.staging_dst);
        r->surf.staging_dst = NULL;
        r->surf.staging_dst_size = 0;
    }
}

void pgraph_wgpu_surface_flush(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;

    // Clear last surface shape to force recreation of buffers at next draw
    pg->surface_color.draw_dirty = false;
    pg->surface_zeta.draw_dirty = false;
    memset(&pg->last_surface_shape, 0, sizeof(pg->last_surface_shape));
    unbind_surface(d, true);
    unbind_surface(d, false);

    SurfaceBinding *s, *next;
    QTAILQ_FOREACH_SAFE(s, &r->surf.surfaces, entry, next) {
        // FIXME: We should download all surfaces to ram, but need to
        //        investigate corruption issue
        pgraph_wgpu_dl_reason = "flush-all";
        pgraph_wgpu_surface_download_if_dirty(d, s);
        invalidate_surface(d, s);
    }
    prune_invalid_surfaces(r, 0);

    pgraph_wgpu_reload_surface_scale_factor(pg);
}
