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
    hwaddr surface_end = surface->vram_addr +
                         pgraph_wgpu_surface_memory_size(surface);
    hwaddr range_end = range_start + range_len;
    return range_len &&
           !(surface->vram_addr >= range_end || range_start >= surface_end);
}

static void wb_flush_range(NV2AState *d, hwaddr start, hwaddr size,
                           const char *why);
static void wb_poll(NV2AState *d);

void pgraph_wgpu_download_surfaces_in_range_if_dirty(PGRAPHState *pg,
                                                     hwaddr start, hwaddr size)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;
    SurfaceBinding *surface;

    wb_flush_range(container_of(pg, NV2AState, pgraph), start, size,
                   "tex/vtx");

    QTAILQ_FOREACH(surface, &r->surf.surfaces, entry) {
        if (check_surface_overlaps_range(surface, start, size)) {
            pgraph_wgpu_surface_download_if_dirty(
                container_of(pg, NV2AState, pgraph), surface);
        }
    }
}

/* Encode the copy of @surface into @staging at @offset; returns its stride. */
static size_t encode_surface_download(PGRAPHState *pg, SurfaceBinding *surface,
                                      WGPUCommandEncoder enc,
                                      WGPUBuffer *staging, size_t offset)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;
    unsigned int width = surface->width, height = surface->height;
    size_t staging_stride;

    if (surface->host_fmt.conv == WGPU_SURFACE_CONV_Z24S8) {
        size_t packed_size = (size_t)width * height * 4;
        assert(offset == 0);
        staging_stride = width * 4;
        pgraph_wgpu_pack_depth_stencil(pg, surface, enc);
        *staging = ensure_staging_dst(r, packed_size);
        wgpuCommandEncoderCopyBufferToBuffer(enc, r->surf.compute.pack_dst, 0,
                                             *staging, 0, packed_size);
    } else {
        staging_stride =
            ROUND_UP(width * surface->host_fmt.host_bytes_per_pixel, 256);
        WGPUTexelCopyTextureInfo src = {
            .texture = surface->texture,
            .aspect = surface->color ? WGPUTextureAspect_All :
                                       WGPUTextureAspect_DepthOnly,
        };
        WGPUTexelCopyBufferInfo dst = {
            .layout = { .offset = offset,
                        .bytesPerRow = staging_stride,
                        .rowsPerImage = height },
            .buffer = *staging,
        };
        wgpuCommandEncoderCopyTextureToBuffer(
            enc, &src, &dst, &(WGPUExtent3D){ width, height, 1 });
    }
    return staging_stride;
}

/* Convert the read-back host image of @surface into guest layout. */
static void store_surface_download(SurfaceBinding *surface, uint8_t *pixels,
                                   const uint8_t *host, size_t staging_stride)
{
    unsigned int width = surface->width, height = surface->height;
    uint8_t *gl_read_buf = pixels;
    uint8_t *swizzle_buf = pixels;

    if (surface->swizzle) {
        // FIXME: Swizzle in shader
        swizzle_buf = (uint8_t *)g_malloc(surface->size);
        gl_read_buf = swizzle_buf;
    }

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

/* ---- early readback of small surfaces the CPU reads back ---- */

static void on_eager_mapped(WGPUMapAsyncStatus status, WGPUStringView msg,
                            void *u1, void *u2)
{
    *(int *)u1 = status == WGPUMapAsyncStatus_Success ? 1 : -1;
}

static void eager_drop(SurfaceBinding *surface)
{
    if (surface->eager_buf) {
        /* the status word is leaked on purpose: a callback may still fire */
        wgpuBufferRelease(surface->eager_buf);
        surface->eager_buf = NULL;
        surface->eager_status = NULL;
    }
}

/*
 * Games recreate small read-back targets every frame (R6's 40x30 one is
 * evicted and cleared each frame), so remember which addresses the CPU read
 * after a GPU draw, not just which SurfaceBinding.
 */
static struct { hwaddr addr; unsigned w, h; } eager_hot[8];
static unsigned eager_hot_next;

static void eager_mark_hot(const SurfaceBinding *s)
{
    for (int i = 0; i < ARRAY_SIZE(eager_hot); i++) {
        if (eager_hot[i].addr == s->vram_addr && eager_hot[i].w == s->width &&
            eager_hot[i].h == s->height) {
            return;
        }
    }
    eager_hot[eager_hot_next % ARRAY_SIZE(eager_hot)].addr = s->vram_addr;
    eager_hot[eager_hot_next % ARRAY_SIZE(eager_hot)].w = s->width;
    eager_hot[eager_hot_next % ARRAY_SIZE(eager_hot)].h = s->height;
    eager_hot_next++;
}

static bool eager_is_hot(const SurfaceBinding *s)
{
    if (s->cpu_read_hot) {
        return true;
    }
    for (int i = 0; i < ARRAY_SIZE(eager_hot); i++) {
        if (eager_hot[i].addr == s->vram_addr && eager_hot[i].w == s->width &&
            eager_hot[i].h == s->height && eager_hot[i].w) {
            return true;
        }
    }
    return false;
}

static bool eager_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        const char *e = getenv("XEMU_WASM_EAGER_READBACK");
        enabled = !(e && *e == '0');
    }
    return enabled;
}

/*
 * @surface stops being the color target. If the CPU read it back after an
 * earlier GPU draw, start copying it back now (submitting the work so far)
 * so the CPU's next read finds the data ready instead of waiting for the
 * whole frame's GPU work.
 */
static void eager_readback_schedule(NV2AState *d, SurfaceBinding *surface)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;

    if (!eager_enabled() || !surface || !surface->color ||
        !eager_is_hot(surface) || !surface->draw_dirty || surface->backing ||
        !surface->texture || surface->host_fmt.conv == WGPU_SURFACE_CONV_Z24S8 ||
        (size_t)surface->width * surface->height > 128 * 128 ||
        r->draw.in_render_pass || r->draw.in_draw) {
        return;
    }
    eager_drop(surface);

    size_t stride = ROUND_UP(surface->width *
                             surface->host_fmt.host_bytes_per_pixel, 256);
    size_t size = stride * surface->height;
    WGPUBuffer buf = wgpuDeviceCreateBuffer(
        r->device, &(WGPUBufferDescriptor){
                       .label = { "eager readback", WGPU_STRLEN },
                       .usage = WGPUBufferUsage_MapRead |
                                WGPUBufferUsage_CopyDst,
                       .size = size });
    WGPUCommandEncoder enc = pgraph_wgpu_begin_nondraw_commands(pg);
    encode_surface_download(pg, surface, enc, &buf, 0);
    pgraph_wgpu_end_nondraw_commands(pg, enc);
    pgraph_wgpu_finish(pg, WGPU_FINISH_REASON_SURFACE_DOWN);

    int *status = g_new0(int, 1);
    surface->eager_future = wgpuBufferMapAsync(
        buf, WGPUMapMode_Read, 0, size,
        (WGPUBufferMapCallbackInfo){ .mode = WGPUCallbackMode_WaitAnyOnly,
                                     .callback = on_eager_mapped,
                                     .userdata1 = status });
    surface->eager_buf = buf;
    surface->eager_status = status;
    surface->eager_stride = stride;
    surface->eager_size = size;
    surface->eager_epoch = surface->gpu_epoch;
    XSTAT_INC(n_eager_readback);
}

/*
 * PFIFO ran out of commands (pfifo.c): a game that draws a small target and
 * then waits for the GPU before reading it is now waiting. Start the bound
 * color target's early copy here; the CPU read finds it done or in flight.
 */
void pgraph_wgpu_on_fifo_idle(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;

    if (r && !QTAILQ_EMPTY(&r->surf.pending_wb)) {
        /* apply deferred write-backs whose copies have landed */
        qemu_mutex_lock(&pg->lock);
        wb_poll(d);
        qemu_mutex_unlock(&pg->lock);
    }
    if (!r || !eager_enabled() || !qatomic_read(&r->color_binding)) {
        return;
    }
    qemu_mutex_lock(&pg->lock);
    SurfaceBinding *s = r->color_binding;
    if (s && s->draw_dirty && eager_is_hot(s) && !r->draw.in_draw &&
        !(s->eager_buf && s->eager_epoch == s->gpu_epoch) &&
        (size_t)s->width * s->height <= 128 * 128) {
        pgraph_wgpu_ensure_not_in_render_pass(pg);
        eager_readback_schedule(d, s);
    }
    qemu_mutex_unlock(&pg->lock);
}

/* Use the early copy of @surface if it is still current; false otherwise. */
static bool eager_readback_take(PGRAPHWgpuState *r, SurfaceBinding *surface,
                                uint8_t *pixels)
{
    if (!surface->eager_buf) {
        return false;
    }
    if (surface->eager_epoch != surface->gpu_epoch) {
        eager_drop(surface);            /* drawn again since: stale */
        return false;
    }
    if (*surface->eager_status == 0) {
        pgraph_wgpu_wait(r, surface->eager_future);   /* usually done */
    }
    bool ok = *surface->eager_status == 1;
    if (ok) {
        g_autofree uint8_t *host = g_malloc(surface->eager_size);
        memcpy(host, wgpuBufferGetConstMappedRange(surface->eager_buf, 0,
                                                   surface->eager_size),
               surface->eager_size);
        wgpuBufferUnmap(surface->eager_buf);
        store_surface_download(surface, pixels, host, surface->eager_stride);
        g_free(surface->eager_status);
        surface->eager_status = NULL;
        XSTAT_INC(n_eager_readback_hit);
    }
    wgpuBufferRelease(surface->eager_buf);
    surface->eager_buf = NULL;
    surface->eager_status = NULL;
    return ok;
}

static void download_surface_to_buffer(NV2AState *d, SurfaceBinding *surface,
                                       uint8_t *pixels)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;

    if (!surface->width || !surface->height) {
        return;
    }

    if (eager_readback_take(r, surface, pixels)) {
        return;     /* the early copy was current: no GPU round trip */
    }

    nv2a_profile_inc_counter(NV2A_PROF_SURF_DOWNLOAD);

    trace_nv2a_pgraph_surface_download(
        surface->color ? "COLOR" : "ZETA",
        surface->swizzle ? "sz" : "lin", surface->vram_addr,
        surface->width, surface->height, surface->pitch,
        surface->fmt.bytes_per_pixel);

    /*
     * Record the copy after any pending draws into the open encoder, then
     * submit everything and wait (pgraph_wgpu_finish) before mapping.
     */
    WGPUCommandEncoder enc = pgraph_wgpu_begin_nondraw_commands(pg);
    WGPUBuffer staging = NULL;
    if (surface->host_fmt.conv != WGPU_SURFACE_CONV_Z24S8) {
        staging = ensure_staging_dst(r,
            ROUND_UP(surface->width * surface->host_fmt.host_bytes_per_pixel,
                     256) * surface->height);
    }
    size_t staging_stride = encode_surface_download(pg, surface, enc,
                                                    &staging, 0);
    pgraph_wgpu_end_nondraw_commands(pg, enc);

    nv2a_profile_inc_counter(NV2A_PROF_QUEUE_SUBMIT_1);
    pgraph_wgpu_finish(pg, WGPU_FINISH_REASON_SURFACE_DOWN);

    size_t read_size = staging_stride * surface->height;
    g_autofree uint8_t *host = g_malloc(read_size);
    XSTAT_INC(n_surf_download);
    XSTAT_ADD(b_surf_download, read_size);
    pgraph_wgpu_read_buffer_sync(r, staging, 0, read_size, host);

    store_surface_download(surface, pixels, host, staging_stride);
}

static void register_cpu_access_callback(NV2AState *d, SurfaceBinding *surface);
static void unregister_cpu_access_callback(NV2AState *d,
                                           SurfaceBinding *surface);
static void destroy_surface_image(PGRAPHWgpuState *r, SurfaceBinding *surface);

static void copy_surface_rect(PGRAPHState *pg, const SurfaceBinding *src,
                               const SurfaceBinding *dst,
                               unsigned int width, unsigned int height,
                               unsigned int dst_x, unsigned int dst_y)
{
    assert(src->texture != dst->texture);
    WGPUCommandEncoder enc = pgraph_wgpu_begin_nondraw_commands(pg);
    WGPUTexelCopyTextureInfo from = {
        .texture = src->texture, .aspect = WGPUTextureAspect_All,
    };
    WGPUTexelCopyTextureInfo to = {
        .texture = dst->texture, .aspect = WGPUTextureAspect_All,
        .origin = { dst_x, dst_y, 0 },
    };
    wgpuCommandEncoderCopyTextureToTexture(
        enc, &from, &to, &(WGPUExtent3D){ width, height, 1 });
    pgraph_wgpu_end_nondraw_commands(pg, enc);
}

void pgraph_wgpu_merge_surface_backing(PGRAPHState *pg,
                                      SurfaceBinding *surface)
{
    if (surface->backing) {
        assert(!surface->backing->backing && surface->draw_dirty);
        if (!surface->swizzle && surface->pitch != surface->backing->pitch) {
            assert(pgraph_wgpu_is_linear_bgra(surface->backing));
            pgraph_wgpu_reshape_surface(pg, surface, surface->backing->view,
                                        surface->backing->vram_addr,
                                        surface->backing->width,
                                        surface->backing->height, false);
        } else {
            copy_surface_rect(pg, surface, surface->backing,
                              surface->width, surface->height, 0, 0);
        }
    }
}

const char *pgraph_wgpu_dl_reason;

/* ---- deferred VRAM write-back of evicted surfaces ---- */

/*
 * Evicting a GPU-dirty surface (an incompatible rebind, an overlapping new
 * target, a retained owner's tail) used to read it back synchronously: the
 * GPU thread waited for all queued GPU work, and games that wait on a GPU
 * fence (D3D's back-end semaphore) waited with it. Rainbow Six 3 does two or
 * three of these per frame. Now the copy is queued with a map, and the bytes
 * are written to VRAM when it completes, or earlier when something touches
 * them: a CPU access (trap), or the emulator reading or writing that VRAM
 * (texture/vertex/command reads, surface uploads and downloads).
 * XEMU_WASM_DEFER_WB=1 enables it (opt-in while it is being validated).
 */
struct PendingWriteback {
    QTAILQ_ENTRY(PendingWriteback) entry;
    SurfaceBinding snap;            /* layout/format of the evicted image */
    hwaddr addr;
    size_t len;
    WGPUBuffer buf;
    WGPUFuture future;
    int *status;
    size_t stride, size;
    MemAccessCallback *cb;
};

static bool wb_defer_ok;            /* set around eviction downloads */

static bool wb_enabled(void)
{
#ifdef EMSCRIPTEN
    static int enabled = -1;
    if (enabled < 0) {
        /* opt-in (=1) until it has run clean in the browser */
        const char *e = getenv("XEMU_WASM_DEFER_WB");
        enabled = e && *e == '1';
    }
    return enabled && tcg_enabled();
#else
    return false;
#endif
}

static void wb_access_callback(void *opaque, MemoryRegion *mr, hwaddr addr,
                               hwaddr len, bool write)
{
    NV2AState *d = (NV2AState *)opaque;
    PGRAPHWgpuState *r = d->pgraph.wgpu_renderer_state;

    /* vCPU: the GPU thread owns WebGPU; have it apply the pending copies */
    qemu_mutex_lock(&d->pfifo.lock);
    qemu_event_reset(&r->surf.downloads_complete);
    qatomic_set(&r->surf.wb_flush_requested, true);
    qatomic_set(&r->surf.downloads_pending, true);
    pfifo_kick(d);
    qemu_mutex_unlock(&d->pfifo.lock);
    XSTAT_T0();
    qemu_event_wait(&r->surf.downloads_complete);
    XSTAT_T1(ns_vcpu_surface_wait);
}

/* GPU thread, pgraph.lock held: write @wb's bytes to VRAM and drop it. */
static void wb_apply(NV2AState *d, PendingWriteback *wb, const char *why)
{
    PGRAPHWgpuState *r = d->pgraph.wgpu_renderer_state;

    if (*wb->status == 0) {
#ifdef EMSCRIPTEN
        char key[64];
        snprintf(key, sizeof(key), "wb:wait %s", why);
        xemu_wasm_count(g_intern_string(key));
#endif
        pgraph_wgpu_wait(r, wb->future);
    }
    if (*wb->status == 1) {
        const uint8_t *host = wgpuBufferGetConstMappedRange(wb->buf, 0,
                                                            wb->size);
        store_surface_download(&wb->snap, d->vram_ptr + wb->addr, host,
                               wb->stride);
        wgpuBufferUnmap(wb->buf);
        memory_region_set_client_dirty(d->vram, wb->addr, wb->len,
                                       DIRTY_MEMORY_VGA);
        memory_region_set_client_dirty(d->vram, wb->addr, wb->len,
                                       DIRTY_MEMORY_NV2A_TEX);
        g_free(wb->status);
    }   /* else: the status word may still be written; leak it like eager */
    wgpuBufferRelease(wb->buf);
    if (wb->cb) {
        mem_access_callback_remove_by_ref(qemu_get_cpu(0), wb->cb);
    }
    QTAILQ_REMOVE(&r->surf.pending_wb, wb, entry);
    g_free(wb);
}

/*
 * Apply every pending write-back overlapping [start, start+size), oldest
 * first, together with all older ones (they may overlap each other).
 */
static void wb_flush_range(NV2AState *d, hwaddr start, hwaddr size,
                           const char *why)
{
    PGRAPHWgpuState *r = d->pgraph.wgpu_renderer_state;
    PendingWriteback *wb, *last = NULL;

    if (!r || QTAILQ_EMPTY(&r->surf.pending_wb)) {
        return;
    }
    QTAILQ_FOREACH(wb, &r->surf.pending_wb, entry) {
        if (wb->addr < start + size && start < wb->addr + wb->len) {
            last = wb;
        }
    }
    while (last) {
        PendingWriteback *first = QTAILQ_FIRST(&r->surf.pending_wb);
        bool done = first == last;
        wb_apply(d, first, why);
        if (done) {
            break;
        }
    }
}

static void wb_flush_all(NV2AState *d)
{
    PGRAPHWgpuState *r = d->pgraph.wgpu_renderer_state;
    while (!QTAILQ_EMPTY(&r->surf.pending_wb)) {
        wb_apply(d, QTAILQ_FIRST(&r->surf.pending_wb), "all");
    }
}

/* GPU thread: apply the copies that have landed, in order, without waiting. */
static void wb_poll(NV2AState *d)
{
    PGRAPHWgpuState *r = d->pgraph.wgpu_renderer_state;
    PendingWriteback *wb;

    while ((wb = QTAILQ_FIRST(&r->surf.pending_wb))) {
        if (*wb->status == 0) {
            WGPUFutureWaitInfo info = { .future = wb->future };
            wgpuInstanceWaitAny(r->instance, 1, &info, 0);
        }
        if (*wb->status == 0) {
            break;
        }
        wb_apply(d, wb, "poll");
    }
}

/* Queue the readback of @image (pgraph.lock held); false: do it synchronously */
static bool wb_defer(NV2AState *d, SurfaceBinding *image)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;

    if (!wb_defer_ok || !wb_enabled() || image->eager_buf ||
        image->host_fmt.conv == WGPU_SURFACE_CONV_Z24S8 || !image->texture ||
        r->draw.in_render_pass || r->draw.in_draw) {
        return false;
    }
    /* an older pending copy of these bytes must land first */
    wb_flush_range(d, image->vram_addr, image->pitch * image->height,
                   "redefer");

    PendingWriteback *wb = g_new0(PendingWriteback, 1);
    wb->stride = ROUND_UP(image->width * image->host_fmt.host_bytes_per_pixel,
                          256);
    wb->size = wb->stride * image->height;
    wb->buf = wgpuDeviceCreateBuffer(
        r->device, &(WGPUBufferDescriptor){
                       .label = { "deferred writeback", WGPU_STRLEN },
                       .usage = WGPUBufferUsage_MapRead |
                                WGPUBufferUsage_CopyDst,
                       .size = wb->size });
    WGPUCommandEncoder enc = pgraph_wgpu_begin_nondraw_commands(pg);
    encode_surface_download(pg, image, enc, &wb->buf, 0);
    pgraph_wgpu_end_nondraw_commands(pg, enc);
    pgraph_wgpu_finish(pg, WGPU_FINISH_REASON_SURFACE_DOWN);
    wb->status = g_new0(int, 1);
    wb->future = wgpuBufferMapAsync(
        wb->buf, WGPUMapMode_Read, 0, wb->size,
        (WGPUBufferMapCallbackInfo){ .mode = WGPUCallbackMode_WaitAnyOnly,
                                     .callback = on_eager_mapped,
                                     .userdata1 = wb->status });
    wb->snap = *image;
    wb->addr = image->vram_addr;
    wb->len = image->pitch * image->height;
    wb->cb = mem_access_callback_insert(qemu_get_cpu(0), d->vram, wb->addr,
                                        wb->len, &wb_access_callback, d);
    QTAILQ_INSERT_TAIL(&r->surf.pending_wb, wb, entry);
    XSTAT_INC(n_surf_download);
    XSTAT_ADD(b_surf_download, wb->size);
#ifdef EMSCRIPTEN
    char key[96];
    snprintf(key, sizeof(key), "dl:deferred %s %ux%u",
             image->color ? "color" : "zeta", image->width, image->height);
    xemu_wasm_count(g_intern_string(key));
#endif
    return true;
}

static void download_surface(NV2AState *d, SurfaceBinding *surface, bool force)
{
    if (!(surface->download_pending || force) || !surface->width ||
        !surface->height) {
        return;
    }
    pgraph_wgpu_merge_surface_backing(&d->pgraph, surface);
    SurfaceBinding *image = surface->backing ? surface->backing : surface;
#ifdef EMSCRIPTEN
    {
        char key[128];
        snprintf(key, sizeof(key), "dl:%s %s %ux%u fmt%u%s",
                 pgraph_wgpu_dl_reason ? pgraph_wgpu_dl_reason : "?",
                 image->color ? "color" : "zeta", image->width,
                 image->height, image->shape.color_format,
                 surface->backing ? " retained" : "");
        xemu_wasm_count(g_intern_string(key));
    }
#endif

    // FIXME: Respect write enable at last TOU?

    if (!wb_defer(d, image)) {
        wb_flush_range(d, image->vram_addr, image->pitch * image->height,
                       "download");
        download_surface_to_buffer(d, image, d->vram_ptr + image->vram_addr);

        memory_region_set_client_dirty(d->vram, image->vram_addr,
                                       image->pitch * image->height,
                                       DIRTY_MEMORY_VGA);
        memory_region_set_client_dirty(d->vram, image->vram_addr,
                                       image->pitch * image->height,
                                       DIRTY_MEMORY_NV2A_TEX);
    }

    surface->download_pending = false;
    surface->draw_dirty = false;

    if (surface->backing) {
        PGRAPHWgpuState *r = d->pgraph.wgpu_renderer_state;

        /* Raw vertex reads must refresh the VRAM mirror as well. */
        memory_region_set_client_dirty(d->vram, image->vram_addr, image->size,
                                       DIRTY_MEMORY_NV2A);
        /*
         * All tails are now in VRAM. Install the smaller write trap BEFORE
         * removing the full trap: a raw GPU-engine read need not have stopped
         * the CPU, so separate queued remove/insert work would leave a gap.
         */
        MemAccessCallback *old_cb = surface->access_cb;
        surface->access_cb = NULL;
        surface->access_cb_write_only = false;
        surface->backing = NULL;
        assert(r->surf.num_retained);
        r->surf.num_retained--;
        if (!surface->upload_pending) {
            register_cpu_access_callback(d, surface);
            if (surface->access_cb) {
                /* Materialization made VRAM current, including the tails. */
                mem_access_callback_set_flags(qemu_get_cpu(0),
                                              surface->access_cb, BP_MEM_WRITE);
                surface->access_cb_write_only = true;
            }
        }
        if (old_cb) {
            mem_access_callback_remove_by_ref(qemu_get_cpu(0), old_cb);
        }
        destroy_surface_image(r, image);
        g_free(image);
    }
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
    if (qatomic_xchg(&r->surf.wb_flush_requested, false)) {
        wb_flush_all(d);
    }
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
    wb_flush_all(d);
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
            if (!write) {
                surface->cpu_read_hot = true;
                eager_mark_hot(surface);
#ifdef EMSCRIPTEN
                if ((size_t)surface->width * surface->height <= 128 * 128) {
                    /* was an early copy in flight for this read? */
                    char key[96];
                    snprintf(key, sizeof(key),
                             "eager:read %ux%u bound%d copy%d current%d",
                             surface->width, surface->height,
                             surface == r->color_binding,
                             surface->eager_buf != NULL,
                             surface->eager_buf &&
                             surface->eager_epoch == surface->gpu_epoch);
                    xemu_wasm_count(g_intern_string(key));
                }
#endif
            }
        }

        if (write) {
            surface->upload_pending = true;
        }

        /*
         * One trapped write per GPU->CPU handover is enough: it marked the
         * surface for re-upload, and keep trapping would make every further
         * CPU write (millions/s when a game writes video frames into a
         * render surface) pay for this callback. A read must keep the trap
         * armed: disarming there would miss a later CPU write, and the GPU
         * would keep using stale contents. Re-armed by
         * pgraph_wgpu_surface_rearm_cpu_trap() once the GPU copy is current
         * again (GPU draw or upload).
         */
        if (write) {
            disarm_cpu_access_callback(d, surface);
        } else if (surface->access_cb && !surface->access_cb_write_only) {
            /*
             * After this read the CPU copy is current (downloaded below if
             * it was not), so further reads need no trap until the GPU
             * draws again: they cost a callback and pgraph.lock each
             * (~20k/s in Rainbow Six 3). Keep trapping writes.
             */
            mem_access_callback_set_flags(qemu_get_cpu(0), surface->access_cb,
                                          BP_MEM_WRITE);
            surface->access_cb_write_only = true;
        }
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
        if (surface->access_cb_write_only) {
            /* the GPU copy is ahead again: trap reads too */
            mem_access_callback_set_flags(qemu_get_cpu(0), surface->access_cb,
                                          BP_MEM_READ | BP_MEM_WRITE);
            surface->access_cb_write_only = false;
        }
        return; /* already armed */
    }
    if (tcg_enabled()) {
        if (surface->width && surface->height) {
            surface->access_cb = mem_access_callback_insert(
                qemu_get_cpu(0), d->vram, surface->vram_addr,
                pgraph_wgpu_surface_memory_size(surface),
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
    surface->access_cb_write_only = false;
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

/* Detach without making the image recyclable (also used for private owners). */
static void detach_surface(NV2AState *d, SurfaceBinding *surface)
{
    PGRAPHWgpuState *r = d->pgraph.wgpu_renderer_state;

    trace_nv2a_pgraph_surface_invalidated(surface->vram_addr);

    assert(!r->draw.in_render_pass && !r->draw.in_draw);

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
}

static void invalidate_surface(NV2AState *d, SurfaceBinding *surface)
{
    PGRAPHWgpuState *r = d->pgraph.wgpu_renderer_state;

    /* A retained owner must be materialized or handed off, never discarded. */
    assert(!surface->backing);
    /* Keep the existing conservative barrier for ordinary invalidation. */
    pgraph_wgpu_finish(&d->pgraph, WGPU_FINISH_REASON_SURFACE_DOWN);
    eager_drop(surface);
    detach_surface(d, surface);
    QTAILQ_INSERT_HEAD(&r->surf.invalid_surfaces, surface, entry);
}

static bool check_surfaces_overlap(const SurfaceBinding *surface,
                                   const SurfaceBinding *other_surface)
{
    return check_surface_overlaps_range(
        surface, other_surface->vram_addr,
        pgraph_wgpu_surface_memory_size(other_surface));
}

static const char *surface_stitch_reject_reason(NV2AState *d,
                                                const SurfaceBinding *src,
                                                const SurfaceBinding *dst);

static void log_surface_overlap(NV2AState *d, const SurfaceBinding *src,
                                 const SurfaceBinding *dst, bool gpu,
                                 const char *reason)
{
#ifdef EMSCRIPTEN
    /* Decisions, not readback counts. Deltas identify guest byte subranges. */
    char key[320];
    snprintf(key, sizeof(key),
             "dl:overlap-%s:%s old:%c/f%u/%ux%u/p%u/%s/a%u "
             "new:%c/f%u/%ux%u/p%u/%s/a%u delta:%+" PRId64 " own:%zu clr%d",
             gpu ? "gpu" : "fallback", reason,
             src->color ? 'C' : 'Z',
             src->color ? src->shape.color_format : src->shape.zeta_format,
             src->width, src->height, src->pitch, src->swizzle ? "sz" : "lin",
             src->shape.anti_aliasing, dst->color ? 'C' : 'Z',
             dst->color ? dst->shape.color_format : dst->shape.zeta_format,
             dst->width, dst->height, dst->pitch, dst->swizzle ? "sz" : "lin",
             dst->shape.anti_aliasing,
             (int64_t)src->vram_addr - (int64_t)dst->vram_addr,
             pgraph_wgpu_surface_memory_size(src), d->pgraph.clearing);
    xemu_wasm_count(g_intern_string(key));
#endif
}

/*
 * Read back every plain dirty surface that @surface evicts with one submit
 * and one buffer map instead of one round trip each (~0.7 ms in Chrome):
 * Rainbow Six 3 evicts a chain of six bloom targets per frame.
 * XEMU_WASM_BATCH_DOWNLOAD=0 disables.
 */
uint32_t pgraph_wgpu_clear_param;

/*
 * True when the clear that is binding @target overwrites every byte of
 * @old: the clear rect spans full rows of @target and covers @old's range,
 * with every channel of @target's format written. @old's contents are then
 * dead in VRAM, so evicting it needs no download.
 */
static bool clear_overwrites(NV2AState *d, const SurfaceBinding *target,
                             const SurfaceBinding *old)
{
    PGRAPHState *pg = &d->pgraph;
    uint32_t param = pgraph_wgpu_clear_param;
    static int enabled = -1;

    if (enabled < 0) {
        const char *e = getenv("XEMU_WASM_CLEAR_DISCARD");
        enabled = !(e && *e == '0');
    }
    if (!enabled || !pg->clearing || !param || !target->pitch ||
        old->backing) {
        return false;
    }
    if (target->color) {
        uint32_t all = NV097_CLEAR_SURFACE_R | NV097_CLEAR_SURFACE_G |
                       NV097_CLEAR_SURFACE_B | NV097_CLEAR_SURFACE_A;
        if ((param & all) != all) {
            return false;
        }
    } else {
        if (!(param & NV097_CLEAR_SURFACE_Z) ||
            (target->host_fmt.stencil &&
             !(param & NV097_CLEAR_SURFACE_STENCIL))) {
            return false;
        }
    }
    if (target->swizzle || target->shape.anti_aliasing) {
        return false;
    }

    uint32_t rx = pgraph_reg_r(pg, NV_PGRAPH_CLEARRECTX);
    uint32_t ry = pgraph_reg_r(pg, NV_PGRAPH_CLEARRECTY);
    unsigned xmin = GET_MASK(rx, NV_PGRAPH_CLEARRECTX_XMIN);
    unsigned xmax = MIN(GET_MASK(rx, NV_PGRAPH_CLEARRECTX_XMAX),
                        target->width - 1);
    unsigned ymin = GET_MASK(ry, NV_PGRAPH_CLEARRECTY_YMIN);
    unsigned ymax = MIN(GET_MASK(ry, NV_PGRAPH_CLEARRECTY_YMAX),
                        target->height - 1);

    if (xmin != 0 || ymin > ymax ||
        (uint64_t)(xmax + 1) * target->fmt.bytes_per_pixel < target->pitch) {
        return false;
    }
    if (old->vram_addr < target->vram_addr) {
        return false;
    }
    uint64_t rel = old->vram_addr - target->vram_addr;
    uint64_t len = pgraph_wgpu_surface_memory_size(old);
    return rel >= (uint64_t)ymin * target->pitch &&
           rel + len <= (uint64_t)(ymax + 1) * target->pitch;
}

static void discard_cleared(NV2AState *d, const SurfaceBinding *target,
                            SurfaceBinding *old)
{
#ifdef EMSCRIPTEN
    char key[96];
    snprintf(key, sizeof(key), "dl:clear-discard %s %ux%u",
             old->color ? "color" : "zeta", old->width, old->height);
    xemu_wasm_count(g_intern_string(key));
#endif
    old->draw_dirty = false;
    old->download_pending = false;
}

static void download_overlapping_batched(NV2AState *d,
                                         SurfaceBinding const *surface)
{
    static int enabled = -1;
    PGRAPHState *pg = &d->pgraph;
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;
    SurfaceBinding *list[16];
    size_t offs[16], strides[16], total = 0;
    int n = 0;

    if (enabled < 0) {
        const char *e = getenv("XEMU_WASM_BATCH_DOWNLOAD");
        enabled = !(e && *e == '0');
    }
    if (!enabled) {
        return;
    }

    SurfaceBinding *o;
    QTAILQ_FOREACH(o, &r->surf.surfaces, entry) {
        if (n == ARRAY_SIZE(list)) {
            break;
        }
        if (!check_surfaces_overlap(surface, o) || !o->draw_dirty ||
            o->backing || clear_overwrites(d, surface, o) ||
            o->eager_buf ||     /* its early copy needs no round trip */ !o->width || !o->height ||
            o->host_fmt.conv == WGPU_SURFACE_CONV_Z24S8) {
            continue;
        }
        strides[n] = ROUND_UP(o->width * o->host_fmt.host_bytes_per_pixel,
                              256);
        offs[n] = total;
        total += ROUND_UP(strides[n] * o->height, 256);
        list[n++] = o;
    }
    if (n < 2) {
        return;
    }

    WGPUBuffer staging = ensure_staging_dst(r, total);
    WGPUCommandEncoder enc = pgraph_wgpu_begin_nondraw_commands(pg);
    for (int i = 0; i < n; i++) {
        const char *reason = surface_stitch_reject_reason(d, list[i], surface);
        log_surface_overlap(d, list[i], surface, false,
                            reason ? reason : "owner-set-or-context");
        nv2a_profile_inc_counter(NV2A_PROF_SURF_DOWNLOAD);
        encode_surface_download(pg, list[i], enc, &staging, offs[i]);
    }
    pgraph_wgpu_end_nondraw_commands(pg, enc);
    nv2a_profile_inc_counter(NV2A_PROF_QUEUE_SUBMIT_1);
    pgraph_wgpu_finish(pg, WGPU_FINISH_REASON_SURFACE_DOWN);

    g_autofree uint8_t *host = g_malloc(total);
    XSTAT_ADD(n_surf_download, n);
    XSTAT_ADD(b_surf_download, total);
    pgraph_wgpu_read_buffer_sync(r, staging, 0, total, host);

    for (int i = 0; i < n; i++) {
        SurfaceBinding *s = list[i];
        char key[128];

        snprintf(key, sizeof(key), "dl:overlap-evict %s %ux%u fmt%u batched",
                 s->color ? "color" : "zeta", s->width, s->height,
                 s->shape.color_format);
        xemu_wasm_count(g_intern_string(key));
        store_surface_download(s, d->vram_ptr + s->vram_addr,
                               host + offs[i], strides[i]);
        memory_region_set_client_dirty(d->vram, s->vram_addr,
                                       s->pitch * s->height, DIRTY_MEMORY_VGA);
        memory_region_set_client_dirty(d->vram, s->vram_addr,
                                       s->pitch * s->height,
                                       DIRTY_MEMORY_NV2A_TEX);
        s->download_pending = false;
        s->draw_dirty = false;
    }
}

static void invalidate_overlapping_surfaces(NV2AState *d,
                                            SurfaceBinding const *surface)
{
    PGRAPHWgpuState *r = d->pgraph.wgpu_renderer_state;

    download_overlapping_batched(d, surface);

    SurfaceBinding *other_surface, *next_surface;
    QTAILQ_FOREACH_SAFE (other_surface, &r->surf.surfaces, entry,
                         next_surface) {
        if (check_surfaces_overlap(surface, other_surface)) {
            if (other_surface->draw_dirty &&
                clear_overwrites(d, surface, other_surface)) {
                discard_cleared(d, surface, other_surface);
            }
            if (other_surface->draw_dirty) {
                const char *reason = surface_stitch_reject_reason(
                    d, other_surface, surface);
                log_surface_overlap(d, other_surface, surface, false,
                                    reason ? reason : "owner-set-or-context");
            }
            trace_nv2a_pgraph_surface_evict_overlapping(
                other_surface->vram_addr, other_surface->width,
                other_surface->height, other_surface->pitch);
            pgraph_wgpu_dl_reason = "overlap-evict";
            wb_defer_ok = true;
            pgraph_wgpu_surface_download_if_dirty(d, other_surface);
            wb_defer_ok = false;
            /*
             * Materializing a retained owner releases its tail. A target in
             * that tail no longer conflicts with the surviving render image,
             * which may still be bound as the other framebuffer attachment.
             */
            if (check_surfaces_overlap(surface, other_surface)) {
                invalidate_surface(d, other_surface);
            }
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
        if (check_surface_overlaps_range(surface, addr, 1)) {
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

static void eager_drop(SurfaceBinding *surface);

static void destroy_surface_image(PGRAPHWgpuState *r, SurfaceBinding *surface)
{
    assert(!surface->backing && !surface->access_cb);
    eager_drop(surface);
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
    if (s1->backing) {
        /* Do not silently reinterpret the retained byte address mapping. */
        if (s1->swizzle != s2->swizzle ||
            s1->shape.color_format != s2->shape.color_format ||
            s1->shape.anti_aliasing != s2->shape.anti_aliasing) {
            return false;
        }
        strict = true;
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

void pgraph_wgpu_materialize_retained(NV2AState *d, hwaddr addr, hwaddr size,
                                     bool write)
{
    PGRAPHWgpuState *r = d->pgraph.wgpu_renderer_state;
    SurfaceBinding *surface;

    /* raw VRAM readers/writers (blit, display, palettes, semaphores) */
    wb_flush_range(d, addr, size, "raw");
    if (!r->surf.num_retained || !size) {
        return;
    }
    QTAILQ_FOREACH(surface, &r->surf.surfaces, entry) {
        if (!surface->backing ||
            !check_surface_overlaps_range(surface, addr, size)) {
            continue;
        }
        pgraph_wgpu_dl_reason = "retained-raw-access";
        pgraph_wgpu_surface_download_if_dirty(d, surface);
        assert(!surface->backing);
        if (write && check_surface_overlaps_range(surface, addr, size)) {
            surface->upload_pending = true;
            d->pgraph.draw_time++;
        }
    }
}

void pgraph_wgpu_pre_read_command(NV2AState *d, hwaddr addr, hwaddr size)
{
    PGRAPHWgpuState *r = d->pgraph.wgpu_renderer_state;

    /*
     * Same PFIFO worker owns the list/geometry (CPU callbacks only change
     * flags). Avoid another PGRAPH lock for unrelated command buffers. Reset
     * and renderer switching quiesce the worker with PFIFO, already held here.
     */
    if (!QTAILQ_EMPTY(&r->surf.pending_wb)) {
        qemu_mutex_lock(&d->pgraph.lock);
        wb_flush_range(d, addr, size, "cmd");
        qemu_mutex_unlock(&d->pgraph.lock);
    }
    if (!r->surf.num_retained) {
        return;
    }
    SurfaceBinding *surface;
    QTAILQ_FOREACH(surface, &r->surf.surfaces, entry) {
        if (surface->backing &&
            check_surface_overlaps_range(surface, addr, size)) {
            qemu_mutex_lock(&d->pgraph.lock);
            pgraph_wgpu_materialize_retained(d, addr, size, false);
            qemu_mutex_unlock(&d->pgraph.lock);
            return;
        }
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
    /* the VRAM this upload reads may still have a copy on its way */
    wb_flush_range(d, surface->vram_addr,
                   pgraph_wgpu_surface_memory_size(surface), "upload");

    if (surface->backing) {
        /* Preserve tails before any forced upload / pending CPU handover. */
        pgraph_wgpu_surface_download_if_dirty(d, surface);
        assert(!surface->backing);
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
    surface->gpu_epoch++;   /* contents replaced: early readbacks are stale */
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
    target->backing = NULL;
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

/* Allocate before retiring a transfer source so its image cannot be reused. */
static SurfaceBinding *allocate_surface_binding(NV2AState *d,
                                                const SurfaceBinding *target)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;
    SurfaceBinding new_binding = *target;
    SurfaceBinding *surface =
        get_any_compatible_invalid_surface(r, &new_binding);

    if (surface) {
        migrate_surface_image(&new_binding, surface);
    } else {
        surface = g_new(SurfaceBinding, 1);
        create_surface_image(pg, &new_binding);
    }
    *surface = new_binding;
    set_surface_label(pg, surface);
    return surface;
}

/*
 * A tightly packed square 2^n Morton image starts with the complete 2^(n-1)
 * square: the low interleaved x/y bits are identical. Rectangles, row padding,
 * scaling, AA changes and format conversions do NOT get this property here.
 */
static bool is_morton_square(const SurfaceBinding *s)
{
    return s->swizzle && s->color && is_power_of_2(s->width) &&
           s->width == s->height &&
           s->pitch == (uint64_t)s->width * s->fmt.bytes_per_pixel &&
           s->size == (uint64_t)s->pitch * s->height &&
           s->host_fmt.conv == WGPU_SURFACE_CONV_NONE &&
           s->host_fmt.host_bytes_per_pixel == s->fmt.bytes_per_pixel;
}

/* A batched method may still be consuming this PFIFO command window. */
static bool overlaps_active_pushbuffer(NV2AState *d, const SurfaceBinding *s)
{
    uint32_t push1 = d->pfifo.regs[NV_PFIFO_CACHE1_PUSH1];
    if (GET_MASK(push1, NV_PFIFO_CACHE1_PUSH1_MODE) !=
        NV_PFIFO_CACHE1_PUSH1_MODE_DMA) {
        return false;
    }
    uint32_t get = qatomic_read(&d->pfifo.regs[NV_PFIFO_CACHE1_DMA_GET]);
    uint32_t put = qatomic_read(&d->pfifo.regs[NV_PFIFO_CACHE1_DMA_PUT]);
    if (get == put) {
        return false;
    }
    hwaddr instance = GET_MASK(d->pfifo.regs[NV_PFIFO_CACHE1_DMA_INSTANCE],
                              NV_PFIFO_CACHE1_DMA_INSTANCE_ADDRESS) << 4;
    DMAObject dma = nv_dma_load(d, instance);
    hwaddr base = dma.address & 0x07ffffff;

    if (get < put) {
        return check_surface_overlaps_range(s, base + get, put - get);
    }
    /* Conservatively cover both portions of a wrapped command ring. */
    return check_surface_overlaps_range(s, base, put) ||
           (get <= dma.limit &&
            check_surface_overlaps_range(s, base + get,
                                         (hwaddr)dma.limit + 1 - get));
}

static bool is_linear_native_color(const SurfaceBinding *s)
{
    return s->color && !s->swizzle && s->width && s->height &&
           s->fmt.bytes_per_pixel &&
           s->host_fmt.conv == WGPU_SURFACE_CONV_NONE &&
           s->fmt.bytes_per_pixel == s->host_fmt.host_bytes_per_pixel &&
           s->pitch % s->fmt.bytes_per_pixel == 0 &&
           (uint64_t)s->width * s->fmt.bytes_per_pixel <= s->pitch &&
           s->size == (uint64_t)s->pitch * s->height;
}

static bool surface_stitch_enabled(void)
{
#ifdef EMSCRIPTEN
    static int enabled = -1;
    if (enabled < 0) {
        const char *e = getenv("XEMU_WASM_OVERLAP_STITCH");
        enabled = !(e && *e == '0');
    }
    return enabled;
#else
    return false;
#endif
}

static const char *surface_stitch_reject_reason(NV2AState *d,
                                                const SurfaceBinding *src,
                                                const SurfaceBinding *dst)
{
    if (!surface_stitch_enabled()) {
        return "disabled";
    }
    if (!tcg_enabled()) {
        return "cpu-owner";
    }
    if (d->pgraph.clearing) {
        return "clear";
    }
    if (!src->color || !dst->color) {
        return "zeta";
    }
    if (src->backing || dst->backing) {
        return "retained-owner";
    }
    if (!src->initialized || !src->texture || !src->size || !dst->size) {
        return "uninitialized";
    }
    if (src->upload_pending || src->download_pending) {
        return "cpu-pending";
    }
    bool reshape = pgraph_wgpu_linear_reshape_enabled() &&
                   pgraph_wgpu_is_linear_bgra_src(src) &&
                   pgraph_wgpu_is_linear_bgra(dst);
    /* A prior tail materialization may leave a clean owner among the mips. */
    if (!src->draw_dirty && !reshape) {
        return "clean";
    }
    if (!src->access_cb || (src->draw_dirty && src->access_cb_write_only)) {
        return "untrapped";
    }
    bool morton = is_morton_square(src) && is_morton_square(dst);
    bool linear = is_linear_native_color(src) && is_linear_native_color(dst);
    if (d->pgraph.surface_scale_factor != 1 || (!morton && !linear)) {
        return "layout";
    }
    if (src->shape.anti_aliasing != dst->shape.anti_aliasing) {
        return "aa";
    }
    if (src->shape.color_format != dst->shape.color_format ||
        src->host_fmt.format != dst->host_fmt.format ||
        src->fmt.bytes_per_pixel != dst->fmt.bytes_per_pixel) {
        return "format";
    }
    if (linear && src->pitch != dst->pitch && !reshape) {
        return "pitch";
    }
    /* Every old guest byte must fit, not just the intersection. */
    if (src->vram_addr < dst->vram_addr || src->size > dst->size ||
        src->vram_addr - dst->vram_addr > dst->size - src->size) {
        return "partial-owner";
    }
    hwaddr delta = src->vram_addr - dst->vram_addr;
    if (morton) {
        if (delta % src->size) {
            return "morton-alignment";
        }
    } else if (delta % src->fmt.bytes_per_pixel ||
               (!reshape &&
                ((delta % dst->pitch) / src->fmt.bytes_per_pixel + src->width >
                     dst->width ||
                 delta / dst->pitch + src->height > dst->height))) {
        /* Padding is CPU-current, but every GPU pixel must fit in the image. */
        return "pixel-range";
    }
    return NULL;
}

/* A Morton block or a same-pitch linear rectangle, never a pitch reshape. */
static void surface_stitch_origin(const SurfaceBinding *src,
                                  const SurfaceBinding *dst,
                                  unsigned int *x, unsigned int *y)
{
    hwaddr delta = src->vram_addr - dst->vram_addr;
    if (!src->swizzle) {
        *x = (delta % dst->pitch) / src->fmt.bytes_per_pixel;
        *y = delta / dst->pitch;
        return;
    }
    uint64_t offset = delta / src->fmt.bytes_per_pixel;
    *x = *y = 0;
    for (unsigned int bit = 1; bit < dst->width; bit <<= 1) {
        if (offset & 1) {
            *x |= bit;
        }
        offset >>= 1;
        if (offset & 1) {
            *y |= bit;
        }
        offset >>= 1;
    }
    assert(!offset && *x % src->width == 0 && *y % src->height == 0);
    assert(*x + src->width <= dst->width && *y + src->height <= dst->height);
}

/*
 * Stitch complete old owners into a containing color target. Keep the
 * canonical image private, using the existing retained-owner protocol for
 * interior CPU/engine accesses, reports, texture fallbacks and Morton shrinks.
 * This intentionally pays for a second image and one full-target GPU copy;
 * dropping the backing would bypass those raw-VRAM coherency barriers.
 */
static SurfaceBinding *try_stitch_surfaces_gpu(NV2AState *d,
                                              const SurfaceBinding *target,
                                              bool upload)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;
    SurfaceBinding *src, *next;
    unsigned int owners = 0;

    if (!upload || !surface_stitch_enabled() || !tcg_enabled() ||
        pg->clearing || pg->surface_scale_factor != 1 || !target->size ||
        (!is_morton_square(target) && !is_linear_native_color(target))) {
        return NULL;
    }
    QTAILQ_FOREACH(src, &r->surf.surfaces, entry) {
        if (!check_surfaces_overlap(target, src)) {
            continue;
        }
        /* All-or-nothing: leave mixed or partially covered owners alone. */
        if (surface_stitch_reject_reason(d, src, target)) {
            return NULL;
        }
        /* No byte may have two candidate owners, regardless of list order. */
        SurfaceBinding *prior;
        QTAILQ_FOREACH(prior, &r->surf.surfaces, entry) {
            if (prior == src) {
                break;
            }
            if (check_surfaces_overlap(src, prior)) {
                return NULL;
            }
        }
        owners += src->draw_dirty;
    }
    if (!owners) {
        return NULL;
    }
    WgpuQueryReport *report;
    QSIMPLEQ_FOREACH(report, &r->draw.report_queue, entry) {
        if (!report->clear) {
            return NULL;
        }
    }
    if (overlaps_active_pushbuffer(d, target)) {
        return NULL;
    }

    /* Neither the sources nor the canonical image may be recycled yet. */
    SurfaceBinding *dst = allocate_surface_binding(d, target);
    SurfaceBinding *backing = allocate_surface_binding(d, target);
    assert(dst->texture != backing->texture);
    register_cpu_access_callback(d, dst);
    /* Prefill uncovered bytes. Each stale source block is replaced below. */
    pgraph_wgpu_upload_surface_data(d, backing, false);
    QTAILQ_FOREACH(src, &r->surf.surfaces, entry) {
        if (!check_surfaces_overlap(target, src) || !src->draw_dirty) {
            continue;
        }
        hwaddr delta = src->vram_addr - target->vram_addr;
        bool reshape = !src->swizzle &&
                       (src->pitch != target->pitch ||
                        (delta % target->pitch) / src->fmt.bytes_per_pixel +
                            src->width > target->width);
        if (reshape) {
            pgraph_wgpu_reshape_surface(pg, src, backing->view,
                                        backing->vram_addr, backing->width,
                                        backing->height, false);
        } else {
            unsigned int x, y;
            surface_stitch_origin(src, target, &x, &y);
            copy_surface_rect(pg, src, backing, src->width, src->height, x, y);
        }
        log_surface_overlap(d, src, target, true,
                            reshape ? "linear-reshape" : "whole-owner");
    }
    copy_surface_rect(pg, backing, dst, dst->width, dst->height, 0, 0);
    dst->backing = backing;
    dst->initialized = true;
    dst->upload_pending = false;
    dst->draw_dirty = true;
    r->surf.num_retained++;

    /* Copies are recorded; later queue writes submit before reusing images. */
    QTAILQ_FOREACH_SAFE(src, &r->surf.surfaces, entry, next) {
        if (check_surfaces_overlap(target, src)) {
            assert(!src->backing);
            src->draw_dirty = false;
            detach_surface(d, src);
            QTAILQ_INSERT_HEAD(&r->surf.invalid_surfaces, src, entry);
        }
    }
    surface_put(d, dst);
    return dst;
}

static SurfaceBinding *try_rebind_surface_prefix(NV2AState *d,
                                               SurfaceBinding *src,
                                               const SurfaceBinding *target,
                                               bool upload)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;
    SurfaceBinding *backing = src->backing ? src->backing : src;
    SurfaceBinding *other;

#ifdef EMSCRIPTEN
    static int enabled = -1, swizzle_enabled;
    if (enabled < 0) {
        const char *e = getenv("XEMU_WASM_SWIZZLE_REBIND");
        const char *all = getenv("XEMU_WASM_REBIND_TRANSFER");
        enabled = !(all && *all == '0');
        swizzle_enabled = !(e && *e == '0');
    }
    if (!enabled) {
        return NULL;
    }
#else
    const bool swizzle_enabled = false;
    return NULL;
#endif
    bool morton = swizzle_enabled && is_morton_square(src) &&
                  is_morton_square(target) && is_morton_square(backing) &&
                  target->width <= backing->width &&
                  (src->backing || target->width < src->width);
    bool linear = pgraph_wgpu_linear_reshape_enabled() &&
                  pgraph_wgpu_is_linear_bgra(src) &&
                  pgraph_wgpu_is_linear_bgra(target) &&
                  pgraph_wgpu_is_linear_bgra(backing) &&
                  target->size <= backing->size;
    if (!upload || !tcg_enabled() || pg->surface_scale_factor != 1 ||
        !src->initialized || !src->texture || !src->draw_dirty ||
        !src->access_cb || src->upload_pending || src->download_pending ||
        (!morton && !linear) ||
        src->shape.anti_aliasing != target->shape.anti_aliasing ||
        src->shape.color_format != target->shape.color_format ||
        src->host_fmt.format != target->host_fmt.format ||
        src->fmt.bytes_per_pixel != target->fmt.bytes_per_pixel ||
        src->vram_addr != target->vram_addr) {
        return NULL;
    }
    /* Deferred report writes must never land in a retained tail. */
    WgpuQueryReport *report;
    QSIMPLEQ_FOREACH(report, &r->draw.report_queue, entry) {
        if (!report->clear) {
            return NULL;
        }
    }
    if (overlaps_active_pushbuffer(d, backing)) {
        return NULL;
    }
    QTAILQ_FOREACH(other, &r->surf.surfaces, entry) {
        if (other != src && check_surfaces_overlap(backing, other)) {
            return NULL;
        }
    }

    SurfaceBinding *dst = allocate_surface_binding(d, target);
    assert(dst->texture != src->texture && dst->texture != backing->texture);
    pgraph_wgpu_merge_surface_backing(pg, src);
    if (linear && backing->pitch != dst->pitch) {
        pgraph_wgpu_reshape_surface(pg, backing, dst->view, dst->vram_addr,
                                    dst->width, dst->height, false);
    } else {
        copy_surface_rect(pg, backing, dst, dst->width, dst->height, 0, 0);
    }

    dst->backing = backing;
    dst->initialized = true;
    dst->upload_pending = false;
    dst->draw_dirty = true;
    /* Keep the entire old interval trapped until it has been materialized. */
    register_cpu_access_callback(d, dst);
#ifdef EMSCRIPTEN
    char key[160];
    snprintf(key, sizeof(key),
             "dl:gpu-%s-rebind fmt%u %ux%u->%ux%u backing%ux%u",
             linear ? "linear" : "swizzle", src->shape.color_format,
             src->width, src->height,
             dst->width, dst->height, backing->width, backing->height);
    xemu_wasm_count(g_intern_string(key));
#endif
    /*
     * All copies are recorded after the old render pass. Subsequent GPU
     * copies/draws are ordered in this encoder; a later CPU upload submits
     * before its queue write. Release-only pruning also preserves recorded
     * references. No finish (or query readback) is needed for this handoff.
     */
    if (src == backing) {
        detach_surface(d, src);
        r->surf.num_retained++;
    } else {
        src->backing = NULL;
        src->draw_dirty = false;
        detach_surface(d, src);
        QTAILQ_INSERT_HEAD(&r->surf.invalid_surfaces, src, entry);
    }
    surface_put(d, dst);
    return dst;
}

/* NULL means every dirty source texel has the same address in the target. */
static const char *surface_transfer_reject_reason(NV2AState *d,
                                                 const SurfaceBinding *src,
                                                 const SurfaceBinding *dst,
                                                 bool upload)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;
    SurfaceBinding *other;

#ifdef EMSCRIPTEN
    static int enabled = -1;
    if (enabled < 0) {
        const char *e = getenv("XEMU_WASM_REBIND_TRANSFER");
        enabled = !(e && *e == '0');
    }
    if (!enabled) {
        return "disabled";
    }
#endif
    if (!upload || !tcg_enabled()) {
        return "cpu-owner";
    }
    if (!src->color || !dst->color) {
        return "zeta";
    }
    if (!src->initialized || !src->texture || !src->width || !src->height) {
        return "uninitialized";
    }
    if (src->upload_pending || src->download_pending) {
        return "cpu-pending";
    }
    if (!src->draw_dirty) {
        return "clean";
    }
    if (!src->access_cb) {
        return "untrapped";
    }
    if (src->backing || src->swizzle || dst->swizzle ||
        pg->surface_scale_factor != 1 ||
        src->shape.anti_aliasing != dst->shape.anti_aliasing) {
        return "layout";
    }
    /* Packed 16bpp host images require quantization, not a raw image copy. */
    if (src->shape.color_format != dst->shape.color_format ||
        src->host_fmt.format != dst->host_fmt.format ||
        src->host_fmt.conv != WGPU_SURFACE_CONV_NONE ||
        dst->host_fmt.conv != WGPU_SURFACE_CONV_NONE ||
        src->fmt.bytes_per_pixel != dst->fmt.bytes_per_pixel ||
        src->host_fmt.host_bytes_per_pixel != src->fmt.bytes_per_pixel ||
        dst->host_fmt.host_bytes_per_pixel != dst->fmt.bytes_per_pixel) {
        return "format";
    }
    if (src->vram_addr != dst->vram_addr || src->pitch != dst->pitch ||
        dst->pitch < (uint64_t)dst->width * dst->fmt.bytes_per_pixel) {
        return "pitch";
    }
    if (dst->width < src->width || dst->height < src->height ||
        dst->size < src->size) {
        return "partial-owner";
    }
    QTAILQ_FOREACH(other, &r->surf.surfaces, entry) {
        if (other != src && check_surfaces_overlap(dst, other)) {
            return "other-owner";
        }
    }
    return NULL;
}

static void log_surface_transfer(const SurfaceBinding *src,
                                  const SurfaceBinding *dst,
                                  const char *reason)
{
#ifdef EMSCRIPTEN
    /* Decision counts; the existing download counters still count readbacks. */
    char key[256];
    snprintf(key, sizeof(key),
             "dl:rebind-%s:%s old:%c/f%u/%ux%u/p%u/%s/a%u "
             "new:%c/f%u/%ux%u/p%u/%s/a%u",
             reason ? "fallback" : "gpu", reason ? reason : "whole-owner",
             src->color ? 'C' : 'Z',
             src->color ? src->shape.color_format : src->shape.zeta_format,
             src->width, src->height, src->pitch, src->swizzle ? "sz" : "lin",
             src->shape.anti_aliasing, dst->color ? 'C' : 'Z',
             dst->color ? dst->shape.color_format : dst->shape.zeta_format,
             dst->width, dst->height, dst->pitch, dst->swizzle ? "sz" : "lin",
             dst->shape.anti_aliasing);
    xemu_wasm_count(g_intern_string(key));
#endif
}

/*
 * A linear 32bpp surface whose bytes are reinterpreted at the same address
 * and pitch as the other kind: Z24S8 <-> 32bpp color (BGRA8 host format, so
 * the host bytes are the guest's little-endian words).
 */
static bool zeta_color_compatible(const SurfaceBinding *s)
{
    if (s->swizzle || s->shape.anti_aliasing || s->backing ||
        s->fmt.bytes_per_pixel != 4 ||
        s->pitch != (uint64_t)s->width * 4 || s->pitch % 256) {
        return false;
    }
    return s->color ? s->host_fmt.format == WGPUTextureFormat_BGRA8Unorm &&
                      s->host_fmt.conv == WGPU_SURFACE_CONV_NONE &&
                      s->host_fmt.host_bytes_per_pixel == 4
                    : s->host_fmt.conv == WGPU_SURFACE_CONV_Z24S8;
}

/*
 * The game reuses one buffer as a depth surface and then as a color surface
 * (or back) at the same address while the old one is GPU-dirty. Convert on
 * the GPU instead of a VRAM round trip: zeta -> color packs the depth/stencil
 * words and copies the rows the color surface covers (only rows beyond it
 * are read back to VRAM); color -> zeta unpacks the color bytes plus the
 * VRAM rows below them into the depth/stencil texture with no readback.
 * XEMU_WASM_ZETA_COLOR_GPU=0 disables.
 */
static SurfaceBinding *try_transfer_zeta_color_gpu(NV2AState *d,
                                                   SurfaceBinding *src,
                                                   const SurfaceBinding *target,
                                                   bool upload)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;
    static int enabled = -1;

    if (enabled < 0) {
        const char *e = getenv("XEMU_WASM_ZETA_COLOR_GPU");
        enabled = !(e && *e == '0');
    }
    if (!enabled || !upload || !tcg_enabled() ||
        pg->surface_scale_factor != 1 || src->color == target->color ||
        !zeta_color_compatible(src) || !zeta_color_compatible(target) ||
        src->vram_addr != target->vram_addr || src->pitch != target->pitch ||
        src->width != target->width || !src->initialized || !src->texture ||
        !src->draw_dirty || src->upload_pending || src->download_pending) {
        return NULL;
    }
    /* zeta -> color must fit inside the zeta; color -> zeta may extend it */
    if (target->color && target->height > src->height) {
        return NULL;
    }
    WgpuQueryReport *report;
    QSIMPLEQ_FOREACH(report, &r->draw.report_queue, entry) {
        if (!report->clear) {
            return NULL;
        }
    }
    if (overlaps_active_pushbuffer(d, src) ||
        overlaps_active_pushbuffer(d, target)) {
        return NULL;
    }
    SurfaceBinding *other;
    QTAILQ_FOREACH(other, &r->surf.surfaces, entry) {
        if (other != src && (check_surfaces_overlap(target, other) ||
                             check_surfaces_overlap(src, other))) {
            return NULL;
        }
    }

    SurfaceBinding *dst = allocate_surface_binding(d, target);
    assert(dst->texture != src->texture);
    size_t row = dst->pitch;

    if (dst->color) {
        WGPUCommandEncoder enc = pgraph_wgpu_begin_nondraw_commands(pg);
        pgraph_wgpu_pack_depth_stencil(pg, src, enc);
        WGPUTexelCopyBufferInfo from = {
            .layout = { .offset = 0, .bytesPerRow = row,
                        .rowsPerImage = dst->height },
            .buffer = r->surf.compute.pack_dst,
        };
        WGPUTexelCopyTextureInfo to = {
            .texture = dst->texture, .aspect = WGPUTextureAspect_All,
        };
        wgpuCommandEncoderCopyBufferToTexture(
            enc, &from, &to, &(WGPUExtent3D){ dst->width, dst->height, 1 });
        size_t tail = row * (src->height - dst->height);
        WGPUBuffer staging = NULL;
        if (tail) {
            staging = ensure_staging_dst(r, tail);
            wgpuCommandEncoderCopyBufferToBuffer(enc, r->surf.compute.pack_dst,
                                                 row * dst->height, staging, 0,
                                                 tail);
        }
        pgraph_wgpu_end_nondraw_commands(pg, enc);
        if (tail) {
            /* the zeta rows the color surface does not cover go to VRAM */
            hwaddr at = src->vram_addr + row * dst->height;
            pgraph_wgpu_finish(pg, WGPU_FINISH_REASON_SURFACE_DOWN);
            XSTAT_INC(n_surf_download);
            XSTAT_ADD(b_surf_download, tail);
            pgraph_wgpu_read_buffer_sync(r, staging, 0, tail,
                                         d->vram_ptr + at);
            memory_region_set_client_dirty(d->vram, at, tail,
                                           DIRTY_MEMORY_VGA);
            memory_region_set_client_dirty(d->vram, at, tail,
                                           DIRTY_MEMORY_NV2A_TEX);
        }
    } else {
        wb_flush_range(d, src->vram_addr, row * dst->height, "zeta");
        WGPUCommandEncoder enc = pgraph_wgpu_begin_nondraw_commands(pg);
        pgraph_wgpu_zeta_from_color(pg, dst, src, enc,
                                    d->vram_ptr + src->vram_addr +
                                    row * src->height);
        pgraph_wgpu_end_nondraw_commands(pg, enc);
    }

    dst->initialized = true;
    dst->upload_pending = false;
    dst->download_pending = false;
    /* VRAM is stale where @src was drawn: @dst now owns those bytes */
    dst->draw_dirty = true;
    src->draw_dirty = false;
#ifdef EMSCRIPTEN
    {
        char key[96];
        snprintf(key, sizeof(key), "dl:gpu-zeta-color %c%ux%u->%c%ux%u",
                 src->color ? 'C' : 'Z', src->width, src->height,
                 dst->color ? 'C' : 'Z', dst->width, dst->height);
        xemu_wasm_count(g_intern_string(key));
    }
#endif
    invalidate_surface(d, src);
    surface_put(d, dst);
    return dst;
}

static SurfaceBinding *try_transfer_surface_gpu(NV2AState *d,
                                                SurfaceBinding *src,
                                                const SurfaceBinding *target,
                                                bool upload)
{
    PGRAPHState *pg = &d->pgraph;
    SurfaceBinding *prefix = try_rebind_surface_prefix(d, src, target, upload);
    const char *reason;

    if (prefix) {
        return prefix;
    }
    prefix = try_transfer_zeta_color_gpu(d, src, target, upload);
    if (prefix) {
        return prefix;
    }
    reason = surface_transfer_reject_reason(d, src, target, upload);
    if (reason) {
        log_surface_transfer(src, target, reason);
        return NULL;
    }

    SurfaceBinding *dst = allocate_surface_binding(d, target);
    assert(dst->texture != src->texture);

    /*
     * Arm the enlarged interval before reading VRAM, as surface_put/upload
     * normally does. Callbacks take pgraph.lock, held throughout this handoff,
     * so they cannot observe the not-yet-published destination. Keep the source
     * live (and out of the recyclable pool) until its copy has been recorded.
     */
    register_cpu_access_callback(d, dst);
    /*
     * This submits prior work before the queue write. VRAM may be stale in
     * the source rectangle; the following ordered GPU copy replaces it.
     * The uncovered margins are current VRAM (there are no other owners).
     */
    pgraph_wgpu_upload_surface_data(d, dst, false);
    assert(dst->initialized && !dst->upload_pending && !dst->download_pending);

    WGPUCommandEncoder enc = pgraph_wgpu_begin_nondraw_commands(pg);
    WGPUTexelCopyTextureInfo from = {
        .texture = src->texture, .aspect = WGPUTextureAspect_All,
    };
    WGPUTexelCopyTextureInfo to = {
        .texture = dst->texture, .aspect = WGPUTextureAspect_All,
    };
    WGPUExtent3D extent = { src->width, src->height, 1 };
    wgpuCommandEncoderCopyTextureToTexture(enc, &from, &to, &extent);
    pgraph_wgpu_end_nondraw_commands(pg, enc);

    /* VRAM is still stale: transfer, rather than clear, dirty ownership. */
    dst->draw_dirty = src->draw_dirty;
    src->draw_dirty = false;
    log_surface_transfer(src, dst, NULL);
    /* Also submits the copy before the source can enter the reusable pool. */
    invalidate_surface(d, src);
    surface_put(d, dst);
    /* surface_update(upload=true) advances draw_time before texture binding. */
    return dst;
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

        if (color) {
            eager_readback_schedule(d, r->color_binding);
        }
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
        bool transferred = false;

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
                hwaddr color_end = surface->vram_addr +
                                  pgraph_wgpu_surface_memory_size(surface);
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
                SurfaceBinding *replacement = try_transfer_surface_gpu(
                    d, surface, &target, upload);
                if (!replacement) {
                    replacement = try_stitch_surfaces_gpu(d, &target, upload);
                }
                if (replacement) {
                    surface = replacement;
                    transferred = true;
                } else {
                    if (surface->draw_dirty &&
                        clear_overwrites(d, &target, surface)) {
                        discard_cleared(d, &target, surface);
                    }
#ifdef EMSCRIPTEN
                    if (surface->draw_dirty) {
                        /* why a clear did not make this download moot */
                        char key[128];
                        uint32_t rx = pgraph_reg_r(pg, NV_PGRAPH_CLEARRECTX);
                        uint32_t ry = pgraph_reg_r(pg, NV_PGRAPH_CLEARRECTY);
                        snprintf(key, sizeof(key),
                                 "dl:rebind-why %c->%c clr%d p%02x "
                                 "x%u-%u y%u-%u",
                                 surface->color ? 'C' : 'Z',
                                 target.color ? 'C' : 'Z', pg->clearing,
                                 pgraph_wgpu_clear_param & 0xff,
                                 GET_MASK(rx, NV_PGRAPH_CLEARRECTX_XMIN),
                                 GET_MASK(rx, NV_PGRAPH_CLEARRECTX_XMAX),
                                 GET_MASK(ry, NV_PGRAPH_CLEARRECTY_YMIN),
                                 GET_MASK(ry, NV_PGRAPH_CLEARRECTY_YMAX));
                        xemu_wasm_count(g_intern_string(key));
                    }
#endif
                    pgraph_wgpu_dl_reason = "incompatible-rebind";
                    wb_defer_ok = true;
                    pgraph_wgpu_surface_download_if_dirty(d, surface);
                    wb_defer_ok = false;
                    invalidate_surface(d, surface);
                }
            }
        }

        if (should_create) {
            if (!transferred) {
                surface = try_stitch_surfaces_gpu(d, &target, upload);
                if (!surface) {
                    surface = allocate_surface_binding(d, &target);
                    surface_put(d, surface);
                }
            }

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
            /*
             * Shape changes unbind here, before update_surface_part: start
             * the outgoing target's early copy now too (e.g. a bloom chain
             * stepping 40x30 -> 80x30 just before the CPU reads 40x30).
             */
            SurfaceBinding *out = r->color_binding;
            if (out && out->draw_dirty && eager_is_hot(out) &&
                (size_t)out->width * out->height <= 128 * 128 &&
                !(out->eager_buf && out->eager_epoch == out->gpu_epoch)) {
                pgraph_wgpu_ensure_not_in_render_pass(pg);
                eager_readback_schedule(d, out);
            }
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
    QTAILQ_INIT(&r->surf.pending_wb);
    r->surf.num_retained = 0;

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
    pgraph_wgpu_finalize_surface_reshape(pg);

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

    wb_flush_all(d);

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
    assert(!r->surf.num_retained);
    prune_invalid_surfaces(r, 0);

    pgraph_wgpu_reload_surface_scale_factor(pg);
}
