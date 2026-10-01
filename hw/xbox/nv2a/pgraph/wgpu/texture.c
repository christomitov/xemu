/*
 * Geforce NV2A PGRAPH WebGPU Renderer: textures
 *
 * Port of the Vulkan renderer's texture.c (../vk/texture.c).
 *
 * Copyright (c) 2024 Matt Borgerson
 *
 * Based on GL implementation:
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2015 Jannik Vogel
 * Copyright (c) 2018-2024 Matt Borgerson
 *
 * Differences from the Vulkan backend, driven by WebGPU:
 *  - WebGPU has no texture view component swizzle and no 16-bit packed,
 *    luminance or palette formats. Every NV2A format is converted on the CPU
 *    at upload time into a format whose sampled RGBA equals what the vk
 *    backend's (format, component swizzle) pair produces; see
 *    kelvin_color_format_wgpu_map below.
 *  - DXT1/3/5 are uploaded as native BC1/2/3 when the device supports
 *    texture-compression-bc (2D/cube textures with block-aligned base size),
 *    otherwise decompressed on the CPU like vk.
 *  - Queue writes (wgpuQueueWriteTexture) execute before the whole next
 *    submit, i.e. before draws already recorded in the open encoder. Writing
 *    new contents into a texture those draws sample would be a hazard, so a
 *    re-upload always creates a fresh WGPUTexture (the old one is kept alive
 *    by WebGPU refcounting for as long as recorded commands use it).
 *  - Border color (CLAMP_TO_BORDER) is not supported by WebGPU samplers and is
 *    approximated with ClampToEdge. Sampler LOD bias is not supported either.
 *  - Render-to-texture: surfaces are converted into the texture on the GPU,
 *    in order on the open encoder (see copy_surface_to_texture). Surface /
 *    texture format pairs that cannot be expressed that way are written back
 *    to VRAM (download if dirty) and decoded like any other texture.
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

#include "qemu/osdep.h"
#include "hw/xbox/nv2a/pgraph/s3tc.h"
#include "hw/xbox/nv2a/pgraph/swizzle.h"
#include "qemu/fast-hash.h"
#include "qemu/lru.h"
#include "renderer.h"
#include "qemu/xemu-wasm-stats.h"

static void texture_cache_release_node_resources(TextureBinding *snode);

/*
 * Host conversion applied to the decoded texels of a level. The input is the
 * texel layout the vk backend would upload for the format (raw NV2A texels,
 * or the output of pgraph_convert_texture_data() / s3tc decompression); the
 * output must sample to the same RGBA as vk's component-swizzled view.
 */
typedef enum TexConv {
    CONV_COPY,           /* already in host layout */
    CONV_BGRX8,          /* B,G,R,X  -> B,G,R,255 */
    CONV_L8,             /* Y        -> Y,Y,Y,255 */
    CONV_AL8,            /* Y        -> Y,Y,Y,Y */
    CONV_A8,             /* A        -> 255,255,255,A */
    CONV_RG8_RGRG,       /* b0,b1    -> b0,b1,b0,b1 */
    CONV_RG8_RRRG,       /* b0,b1    -> b0,b0,b0,b1 */
    CONV_RG8_GRRG,       /* b0,b1    -> b1,b0,b0,b1 */
    CONV_A1R5G5B5,       /* 16 bit   -> RGBA8 */
    CONV_X1R5G5B5,
    CONV_A4R4G4B4,
    CONV_R5G6B5,
    CONV_RGB8_SNORM,     /* 3 x s8   -> RGBA8 snorm, A = 1.0 */
    CONV_RGBA8_GBAR,     /* b0..b3   -> b1,b2,b3,b0 */
    CONV_RGBA8_ABGR,     /* b0..b3   -> b3,b2,b1,b0 */
    CONV_L16,            /* u16      -> (y,y,y,1) float */
    CONV_DEPTH16_FIXED,  /* u16      -> (d,0,0,0) float */
    CONV_DEPTH16_FLOAT,  /* u16      -> (d,0,1,0) float */
} TexConv;

typedef struct WgpuColorFormatInfo {
    WGPUTextureFormat format;
    TexConv conv;
    /* sampled color can be blitted from a color surface (see
     * check_surface_to_texture_compatiblity) */
    bool from_surface;
    bool from_surface_opaque;
} WgpuColorFormatInfo;

/*
 * Depth textures: vk uses R16_UNORM for Y16 depth and R32_UINT for X8_Y24,
 * with a swizzle carrying "24 bit" in G and "float" in B for the pixel
 * shader's manual shadow compare (glsl/psh.c psh_append_shadowmap). Y16 is
 * stored here as RGBA32Float holding exactly that swizzled vec4 (R16Unorm is
 * not core WebGPU). X8_Y24 stays R32Uint holding the raw VRAM word, matching
 * the "usampler2D ... >> 8" code psh.c emits when opts.vulkan is set.
 */
static const WgpuColorFormatInfo kelvin_color_format_wgpu_map[66] = {
    [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_Y8] =
        { WGPUTextureFormat_RGBA8Unorm, CONV_L8 },
    [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_AY8] =
        { WGPUTextureFormat_RGBA8Unorm, CONV_AL8 },
    [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A1R5G5B5] =
        { WGPUTextureFormat_RGBA8Unorm, CONV_A1R5G5B5, true, false },
    [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_X1R5G5B5] =
        { WGPUTextureFormat_RGBA8Unorm, CONV_X1R5G5B5, true, true },
    [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A4R4G4B4] =
        { WGPUTextureFormat_RGBA8Unorm, CONV_A4R4G4B4 },
    [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R5G6B5] =
        { WGPUTextureFormat_RGBA8Unorm, CONV_R5G6B5, true, true },
    [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8R8G8B8] =
        { WGPUTextureFormat_BGRA8Unorm, CONV_COPY, true, false },
    [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_X8R8G8B8] =
        { WGPUTextureFormat_BGRA8Unorm, CONV_BGRX8, true, true },
    [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_I8_A8R8G8B8] =
        { WGPUTextureFormat_BGRA8Unorm, CONV_COPY }, /* palette converted */
    [NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT1_A1R5G5B5] =
        { WGPUTextureFormat_RGBA8Unorm, CONV_COPY }, /* or BC1 */
    [NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT23_A8R8G8B8] =
        { WGPUTextureFormat_RGBA8Unorm, CONV_COPY }, /* or BC2 */
    [NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT45_A8R8G8B8] =
        { WGPUTextureFormat_RGBA8Unorm, CONV_COPY }, /* or BC3 */
    [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A1R5G5B5] =
        { WGPUTextureFormat_RGBA8Unorm, CONV_A1R5G5B5, true, false },
    [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_R5G6B5] =
        { WGPUTextureFormat_RGBA8Unorm, CONV_R5G6B5, true, true },
    [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8R8G8B8] =
        { WGPUTextureFormat_BGRA8Unorm, CONV_COPY, true, false },
    [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_Y8] =
        { WGPUTextureFormat_RGBA8Unorm, CONV_L8 },
    [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_G8B8] =
        { WGPUTextureFormat_RGBA8Unorm, CONV_RG8_RGRG },
    [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8] =
        { WGPUTextureFormat_RGBA8Unorm, CONV_A8 },
    [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8Y8] =
        { WGPUTextureFormat_RGBA8Unorm, CONV_RG8_RRRG },
    [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_AY8] =
        { WGPUTextureFormat_RGBA8Unorm, CONV_AL8 },
    [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_X1R5G5B5] =
        { WGPUTextureFormat_RGBA8Unorm, CONV_X1R5G5B5, true, true },
    [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A4R4G4B4] =
        { WGPUTextureFormat_RGBA8Unorm, CONV_A4R4G4B4 },
    [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_X8R8G8B8] =
        { WGPUTextureFormat_BGRA8Unorm, CONV_BGRX8, true, true },
    [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8] =
        { WGPUTextureFormat_RGBA8Unorm, CONV_A8 },
    [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8Y8] =
        { WGPUTextureFormat_RGBA8Unorm, CONV_RG8_RRRG },
    [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R6G5B5] =
        { WGPUTextureFormat_RGBA8Snorm, CONV_RGB8_SNORM }, /* converted */
    [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_G8B8] =
        { WGPUTextureFormat_RGBA8Unorm, CONV_RG8_RGRG },
    [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R8B8] =
        { WGPUTextureFormat_RGBA8Unorm, CONV_RG8_GRRG },
    [NV097_SET_TEXTURE_FORMAT_COLOR_LC_IMAGE_CR8YB8CB8YA8] =
        { WGPUTextureFormat_RGBA8Unorm, CONV_COPY }, /* converted */
    [NV097_SET_TEXTURE_FORMAT_COLOR_LC_IMAGE_YB8CR8YA8CB8] =
        { WGPUTextureFormat_RGBA8Unorm, CONV_COPY }, /* converted */
    [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_DEPTH_Y16_FIXED] =
        { WGPUTextureFormat_RGBA32Float, CONV_DEPTH16_FIXED },
    [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_DEPTH_X8_Y24_FIXED] =
        { WGPUTextureFormat_R32Uint, CONV_COPY },
    [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_DEPTH_X8_Y24_FLOAT] =
        { WGPUTextureFormat_R32Uint, CONV_COPY },
    [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_DEPTH_Y16_FIXED] =
        { WGPUTextureFormat_RGBA32Float, CONV_DEPTH16_FIXED },
    [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_DEPTH_Y16_FLOAT] =
        { WGPUTextureFormat_RGBA32Float, CONV_DEPTH16_FLOAT },
    [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_Y16] =
        { WGPUTextureFormat_RGBA32Float, CONV_L16 },
    [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8B8G8R8] =
        { WGPUTextureFormat_RGBA8Unorm, CONV_COPY },
    [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_B8G8R8A8] =
        { WGPUTextureFormat_RGBA8Unorm, CONV_RGBA8_GBAR },
    [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R8G8B8A8] =
        { WGPUTextureFormat_RGBA8Unorm, CONV_RGBA8_ABGR },
    [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8B8G8R8] =
        { WGPUTextureFormat_RGBA8Unorm, CONV_COPY },
    [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_B8G8R8A8] =
        { WGPUTextureFormat_RGBA8Unorm, CONV_RGBA8_GBAR },
    [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_R8G8B8A8] =
        { WGPUTextureFormat_RGBA8Unorm, CONV_RGBA8_ABGR },
};

/* Same as vk's pgraph_texture_min_filter_vk_map (used for mag too, as vk) */
static const WGPUFilterMode pgraph_texture_min_filter_wgpu_map[] = {
    WGPUFilterMode_Nearest,
    WGPUFilterMode_Nearest,
    WGPUFilterMode_Linear,
    WGPUFilterMode_Nearest,
    WGPUFilterMode_Linear,
    WGPUFilterMode_Nearest,
    WGPUFilterMode_Linear,
    WGPUFilterMode_Linear,
};

static const WGPUAddressMode pgraph_texture_addr_wgpu_map[] = {
    0,
    WGPUAddressMode_Repeat,
    WGPUAddressMode_MirrorRepeat,
    WGPUAddressMode_ClampToEdge,
    WGPUAddressMode_ClampToEdge, /* CLAMP_TO_BORDER: no border in WebGPU */
    WGPUAddressMode_ClampToEdge, /* Approximate GL_CLAMP */
};

static WGPUAddressMode lookup_texture_address_mode(int idx)
{
    assert(0 < idx && idx < ARRAY_SIZE(pgraph_texture_addr_wgpu_map));
    return pgraph_texture_addr_wgpu_map[idx];
}

static unsigned int wgpu_format_texel_size(WGPUTextureFormat format)
{
    switch (format) {
    case WGPUTextureFormat_RGBA8Unorm:
    case WGPUTextureFormat_RGBA8Snorm:
    case WGPUTextureFormat_BGRA8Unorm:
    case WGPUTextureFormat_R32Uint:
        return 4;
    case WGPUTextureFormat_RGBA32Float:
        return 16;
    default:
        assert(!"Unexpected texture format");
        return 0;
    }
}

static bool is_linear_filter_supported_for_format(PGRAPHWgpuState *r,
                                                  WGPUTextureFormat format)
{
    switch (format) {
    case WGPUTextureFormat_R32Uint:
        return false;
    case WGPUTextureFormat_RGBA32Float:
        return r->has_float32_filterable;
    default:
        return true;
    }
}

static WGPUTextureSampleType sample_type_for_format(PGRAPHWgpuState *r,
                                                    WGPUTextureFormat format)
{
    if (format == WGPUTextureFormat_R32Uint) {
        return WGPUTextureSampleType_Uint;
    }
    return is_linear_filter_supported_for_format(r, format) ?
               WGPUTextureSampleType_Float :
               WGPUTextureSampleType_UnfilterableFloat;
}

// FIXME: Move to common
// FIXME: We can shrink the size of this structure
typedef struct TextureLevel {
    unsigned int width, height, depth; /* copy extent (physical for BC) */
    unsigned int bytes_per_row, rows_per_image;
    void *decoded_data;
    size_t decoded_size;
} TextureLevel;

typedef struct TextureLayer {
    TextureLevel levels[16];
} TextureLayer;

typedef struct TextureLayout {
    WGPUTextureFormat format;
    unsigned int width, height, depth; /* level 0 size of the texture */
    TextureLayer layers[6];
} TextureLayout;

// FIXME: Move to common
static enum S3TC_DECOMPRESS_FORMAT kelvin_format_to_s3tc_format(int color_format)
{
    switch (color_format) {
    case NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT1_A1R5G5B5:
        return S3TC_DECOMPRESS_FORMAT_DXT1;
    case NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT23_A8R8G8B8:
        return S3TC_DECOMPRESS_FORMAT_DXT3;
    case NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT45_A8R8G8B8:
        return S3TC_DECOMPRESS_FORMAT_DXT5;
    default:
        assert(!"Invalid texture color format");
    }
}

static WGPUTextureFormat kelvin_format_to_bc_format(int color_format)
{
    switch (color_format) {
    case NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT1_A1R5G5B5:
        return WGPUTextureFormat_BC1RGBAUnorm;
    case NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT23_A8R8G8B8:
        return WGPUTextureFormat_BC2RGBAUnorm;
    case NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT45_A8R8G8B8:
        return WGPUTextureFormat_BC3RGBAUnorm;
    default:
        assert(!"Invalid texture color format");
    }
}

// FIXME: Move to common
static void memcpy_image(void *dst, void *src, int min_stride, int dst_stride,
                         int src_stride, int height)
{
    uint8_t *dst_ptr = (uint8_t *)dst;
    uint8_t *src_ptr = (uint8_t *)src;

    for (int i = 0; i < height; i++) {
        memcpy(dst_ptr, src_ptr, min_stride);
        src_ptr += src_stride;
        dst_ptr += dst_stride;
    }
}

static inline uint8_t expand5(unsigned int v)
{
    return (v << 3) | (v >> 2);
}

static inline uint8_t expand6(unsigned int v)
{
    return (v << 2) | (v >> 4);
}

/*
 * Convert @count decoded texels into the host layout of
 * kelvin_color_format_wgpu_map[color_format]. Returns NULL for CONV_COPY.
 */
static uint8_t *convert_texels(int color_format, const uint8_t *src,
                               size_t count, size_t *out_size)
{
    const WgpuColorFormatInfo *wf = &kelvin_color_format_wgpu_map[color_format];

    if (wf->conv == CONV_COPY) {
        return NULL;
    }

    size_t size = count * wgpu_format_texel_size(wf->format);
    uint8_t *out = g_malloc(size);
    uint8_t *o = out;
    float *of = (float *)out;

    for (size_t i = 0; i < count; i++) {
        uint16_t p16;

        switch (wf->conv) {
        case CONV_BGRX8:
            o[0] = src[i * 4 + 0];
            o[1] = src[i * 4 + 1];
            o[2] = src[i * 4 + 2];
            o[3] = 0xff;
            break;
        case CONV_L8:
            o[0] = o[1] = o[2] = src[i];
            o[3] = 0xff;
            break;
        case CONV_AL8:
            o[0] = o[1] = o[2] = o[3] = src[i];
            break;
        case CONV_A8:
            o[0] = o[1] = o[2] = 0xff;
            o[3] = src[i];
            break;
        case CONV_RG8_RGRG:
            o[0] = o[2] = src[i * 2 + 0];
            o[1] = o[3] = src[i * 2 + 1];
            break;
        case CONV_RG8_RRRG:
            o[0] = o[1] = o[2] = src[i * 2 + 0];
            o[3] = src[i * 2 + 1];
            break;
        case CONV_RG8_GRRG:
            o[0] = o[3] = src[i * 2 + 1];
            o[1] = o[2] = src[i * 2 + 0];
            break;
        case CONV_A1R5G5B5:
        case CONV_X1R5G5B5:
            p16 = lduw_le_p(src + i * 2);
            o[0] = expand5((p16 >> 10) & 0x1f);
            o[1] = expand5((p16 >> 5) & 0x1f);
            o[2] = expand5(p16 & 0x1f);
            o[3] = (wf->conv == CONV_X1R5G5B5 || (p16 & 0x8000)) ? 0xff : 0;
            break;
        case CONV_A4R4G4B4:
            p16 = lduw_le_p(src + i * 2);
            o[0] = ((p16 >> 8) & 0xf) * 0x11;
            o[1] = ((p16 >> 4) & 0xf) * 0x11;
            o[2] = (p16 & 0xf) * 0x11;
            o[3] = ((p16 >> 12) & 0xf) * 0x11;
            break;
        case CONV_R5G6B5:
            p16 = lduw_le_p(src + i * 2);
            o[0] = expand5((p16 >> 11) & 0x1f);
            o[1] = expand6((p16 >> 5) & 0x3f);
            o[2] = expand5(p16 & 0x1f);
            o[3] = 0xff;
            break;
        case CONV_RGB8_SNORM:
            o[0] = src[i * 3 + 0];
            o[1] = src[i * 3 + 1];
            o[2] = src[i * 3 + 2];
            o[3] = 0x7f; /* 1.0 */
            break;
        case CONV_RGBA8_GBAR:
            o[0] = src[i * 4 + 1];
            o[1] = src[i * 4 + 2];
            o[2] = src[i * 4 + 3];
            o[3] = src[i * 4 + 0];
            break;
        case CONV_RGBA8_ABGR:
            o[0] = src[i * 4 + 3];
            o[1] = src[i * 4 + 2];
            o[2] = src[i * 4 + 1];
            o[3] = src[i * 4 + 0];
            break;
        case CONV_L16:
            p16 = lduw_le_p(src + i * 2);
            of[0] = of[1] = of[2] = p16 / 65535.0f;
            of[3] = 1.0f;
            break;
        case CONV_DEPTH16_FIXED:
        case CONV_DEPTH16_FLOAT:
            p16 = lduw_le_p(src + i * 2);
            of[0] = p16 / 65535.0f;
            of[1] = 0.0f;
            of[2] = wf->conv == CONV_DEPTH16_FLOAT ? 1.0f : 0.0f;
            of[3] = 0.0f;
            break;
        default:
            g_assert_not_reached();
        }

        if (wf->format == WGPUTextureFormat_RGBA32Float) {
            of += 4;
        } else {
            o += 4;
        }
    }

    *out_size = size;
    return out;
}

/*
 * Finish a decoded level: optionally crop it (cubemap border), convert it to
 * the host format and record it in @level. Takes ownership of @data.
 */
static void finish_level(TextureLevel *level, int color_format,
                         WGPUTextureFormat format, uint8_t *data,
                         unsigned int in_bpp, unsigned int width,
                         unsigned int height, unsigned int depth,
                         unsigned int crop_width, unsigned int crop_height)
{
    if (crop_width != width || crop_height != height) {
        /* FIXME: Consider preserving the border. There does not seem to be
         * a way to reference the border texels in a cubemap, so they are
         * discarded (skip 4 texels/rows like the GL backend). */
        assert(depth == 1);
        unsigned int off_x = MIN(4, width - crop_width);
        unsigned int off_y = MIN(4, height - crop_height);
        uint8_t *cropped = g_malloc(crop_width * crop_height * in_bpp);
        memcpy_image(cropped, data + (off_y * width + off_x) * in_bpp,
                     crop_width * in_bpp, crop_width * in_bpp, width * in_bpp,
                     crop_height);
        g_free(data);
        data = cropped;
        width = crop_width;
        height = crop_height;
    }

    size_t count = (size_t)width * height * depth;
    size_t size = count * in_bpp;
    uint8_t *converted = convert_texels(color_format, data, count, &size);
    if (converted) {
        g_free(data);
        data = converted;
    }

    *level = (TextureLevel){
        .width = width,
        .height = height,
        .depth = depth,
        .bytes_per_row = width * wgpu_format_texel_size(format),
        .rows_per_image = height,
        .decoded_size = size,
        .decoded_data = data,
    };
}

static bool can_use_native_bc(PGRAPHWgpuState *r, TextureShape s,
                              unsigned int adjusted_width,
                              unsigned int adjusted_height)
{
    if (!r->has_bc_compression || s.dimensionality != 2) {
        return false;
    }
    if (s.cubemap && adjusted_width != s.width) {
        return false; /* border is cropped on the CPU */
    }
    /* WebGPU requires block aligned base size for BC textures */
    return (adjusted_width % 4) == 0 && (adjusted_height % 4) == 0;
}

// FIXME: Move to common
// FIXME: More refactoring
// FIXME: Possible parallelization of decoding
// FIXME: Bounds checking
static TextureLayout *get_texture_layout(PGRAPHState *pg, int texture_idx)
{
    NV2AState *d = container_of(pg, NV2AState, pgraph);
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;
    TextureShape s = pgraph_get_texture_shape(pg, texture_idx);
    BasicColorFormatInfo f = kelvin_color_format_info_map[s.color_format];
    WGPUTextureFormat format =
        kelvin_color_format_wgpu_map[s.color_format].format;

    // Sanity checks on below assumptions
    if (f.linear) {
        assert(s.dimensionality == 2);
    }
    if (s.cubemap) {
        assert(s.dimensionality == 2);
        assert(!f.linear);
    }
    assert(s.dimensionality > 1);

    const hwaddr texture_vram_offset =
        pgraph_get_texture_phys_addr(pg, texture_idx);
    uint8_t *texture_data_ptr = (uint8_t *)d->vram_ptr + texture_vram_offset;

    size_t texture_palette_data_size;
    const hwaddr texture_palette_vram_offset =
        pgraph_get_texture_palette_phys_addr_length(pg, texture_idx,
                                                    &texture_palette_data_size);
    void *palette_data_ptr = (char *)d->vram_ptr + texture_palette_vram_offset;

    unsigned int adjusted_width = s.width, adjusted_height = s.height,
                 adjusted_pitch = s.pitch, adjusted_depth = s.depth;

    if (!f.linear && s.border) {
        adjusted_width = MAX(16, adjusted_width * 2);
        adjusted_height = MAX(16, adjusted_height * 2);
        adjusted_pitch = adjusted_width * (s.pitch / s.width);
        adjusted_depth = MAX(16, s.depth * 2);
    }

    TextureLayout *layout = g_malloc0(sizeof(TextureLayout));

    if (f.linear) {
        assert(s.pitch % f.bytes_per_pixel == 0 &&
               "Can't handle strides unaligned to pixels");

        size_t converted_size;
        uint8_t *converted = pgraph_convert_texture_data(
            s, texture_data_ptr, palette_data_ptr, adjusted_width,
            adjusted_height, 1, adjusted_pitch, 0, &converted_size);
        unsigned int in_bpp = converted ?
                                  converted_size / (adjusted_width *
                                                    adjusted_height) :
                                  f.bytes_per_pixel;

        if (!converted) {
            int dst_stride = adjusted_width * f.bytes_per_pixel;
            assert(adjusted_width <= s.width);
            converted_size = dst_stride * adjusted_height;
            converted = g_malloc(converted_size);
            memcpy_image(converted, texture_data_ptr,
                         adjusted_width * f.bytes_per_pixel, dst_stride,
                         adjusted_pitch, adjusted_height);
        }

        assert(s.levels == 1);
        finish_level(&layout->layers[0].levels[0], s.color_format, format,
                     converted, in_bpp, adjusted_width, adjusted_height, 1,
                     adjusted_width, adjusted_height);
        layout->format = format;
        layout->width = adjusted_width;
        layout->height = adjusted_height;
        layout->depth = 1;
        return layout;
    }

    bool is_compressed =
        pgraph_is_texture_format_compressed(pg, s.color_format);
    size_t block_size = 0;
    bool native_bc = false;
    if (is_compressed) {
        bool is_dxt1 =
            s.color_format == NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT1_A1R5G5B5;
        block_size = is_dxt1 ? 8 : 16;
        native_bc = can_use_native_bc(r, s, adjusted_width, adjusted_height);
        if (native_bc) {
            format = kelvin_format_to_bc_format(s.color_format);
        }
    }
    layout->format = format;

    if (s.dimensionality == 2) {
        hwaddr layer_size = 0;
        const int num_layers = s.cubemap ? 6 : 1;
        bool crop = s.cubemap && adjusted_width != s.width;

        if (s.cubemap) {
            // FIXME: Move to common (vk get_cubemap_layer_size)
            unsigned int w = adjusted_width, h = adjusted_height;
            size_t length = 0;
            for (int level = 0; level < s.levels; level++) {
                if (is_compressed) {
                    length += w / 4 * h / 4 * block_size;
                } else {
                    length += w * h * f.bytes_per_pixel;
                }
                w /= 2;
                h /= 2;
            }
            layer_size = ROUND_UP(length, NV2A_CUBEMAP_FACE_ALIGNMENT);
        }

        layout->width = crop ? s.width : adjusted_width;
        layout->height = crop ? s.height : adjusted_height;
        layout->depth = 1;

        for (int layer = 0; layer < num_layers; layer++) {
            unsigned int width = adjusted_width, height = adjusted_height;
            texture_data_ptr = (uint8_t *)d->vram_ptr + texture_vram_offset +
                               layer * layer_size;

            for (int level = 0; level < s.levels; level++) {
                TextureLevel *tl = &layout->layers[layer].levels[level];
                unsigned int crop_width = crop ? MAX(1, s.width >> level) :
                                                 MAX(width, 1);
                unsigned int crop_height = crop ? MAX(1, s.height >> level) :
                                                  MAX(height, 1);

                width = MAX(width, 1);
                height = MAX(height, 1);
                if (is_compressed) {
                    // https://docs.microsoft.com/en-us/windows/win32/direct3d10/d3d10-graphics-programming-guide-resources-block-compression#virtual-size-versus-physical-size
                    unsigned int physical_width = (width + 3) & ~3,
                                 physical_height = (height + 3) & ~3;
                    size_t phys_size =
                        physical_width / 4 * physical_height / 4 * block_size;

                    if (native_bc) {
                        *tl = (TextureLevel){
                            .width = physical_width,
                            .height = physical_height,
                            .depth = 1,
                            .bytes_per_row = physical_width / 4 * block_size,
                            .rows_per_image = physical_height / 4,
                            .decoded_size = phys_size,
                            .decoded_data =
                                g_memdup2(texture_data_ptr, phys_size),
                        };
                    } else {
                        uint8_t *converted = s3tc_decompress_2d(
                            kelvin_format_to_s3tc_format(s.color_format),
                            texture_data_ptr, width, height);
                        assert(converted);
                        finish_level(tl, s.color_format, format, converted, 4,
                                     width, height, 1, crop_width,
                                     crop_height);
                    }

                    texture_data_ptr += phys_size;
                } else {
                    unsigned int pitch = width * f.bytes_per_pixel;

                    size_t converted_size;
                    uint8_t *unswizzled = (uint8_t *)g_malloc(height * pitch);
                    unswizzle_rect(texture_data_ptr, width, height,
                                   unswizzled, pitch, f.bytes_per_pixel);

                    uint8_t *converted = pgraph_convert_texture_data(
                        s, unswizzled, palette_data_ptr, width, height, 1,
                        pitch, 0, &converted_size);
                    unsigned int in_bpp = f.bytes_per_pixel;

                    if (converted) {
                        g_free(unswizzled);
                        in_bpp = converted_size / (width * height);
                    } else {
                        converted = unswizzled;
                    }

                    finish_level(tl, s.color_format, format, converted,
                                 in_bpp, width, height, 1, crop_width,
                                 crop_height);

                    texture_data_ptr += width * height * f.bytes_per_pixel;
                }

                width /= 2;
                height /= 2;
            }
        }
    } else if (s.dimensionality == 3) {
        assert(!f.linear);
        assert(!native_bc);
        unsigned int width = adjusted_width, height = adjusted_height,
                     depth = adjusted_depth;

        layout->width = adjusted_width;
        layout->height = adjusted_height;
        layout->depth = adjusted_depth;

        for (int level = 0; level < s.levels; level++) {
            TextureLevel *tl = &layout->layers[0].levels[level];

            width = MAX(width, 1);
            height = MAX(height, 1);
            depth = MAX(depth, 1);

            if (is_compressed) {
                unsigned int physical_width = (width + 3) & ~3,
                             physical_height = (height + 3) & ~3;

                uint8_t *converted = s3tc_decompress_3d(
                    kelvin_format_to_s3tc_format(s.color_format),
                    texture_data_ptr, width, height, depth);
                assert(converted);

                finish_level(tl, s.color_format, format, converted, 4, width,
                             height, depth, width, height);

                texture_data_ptr += physical_width / 4 * physical_height / 4 *
                                    depth * block_size;
            } else {
                unsigned int row_pitch = width * f.bytes_per_pixel;
                unsigned int slice_pitch = row_pitch * height;

                size_t unswizzled_size = slice_pitch * depth;
                uint8_t *unswizzled = g_malloc(unswizzled_size);
                unswizzle_box(texture_data_ptr, width, height, depth,
                              unswizzled, row_pitch, slice_pitch,
                              f.bytes_per_pixel);

                size_t converted_size;
                uint8_t *converted = pgraph_convert_texture_data(
                    s, unswizzled, palette_data_ptr, width, height, depth,
                    row_pitch, slice_pitch, &converted_size);
                unsigned int in_bpp = f.bytes_per_pixel;

                if (converted) {
                    g_free(unswizzled);
                    in_bpp = converted_size / (width * height * depth);
                } else {
                    converted = unswizzled;
                }

                finish_level(tl, s.color_format, format, converted, in_bpp,
                             width, height, depth, width, height);

                texture_data_ptr += width * height * depth * f.bytes_per_pixel;
            }

            width /= 2;
            height /= 2;
            depth /= 2;
        }
    }

    return layout;
}

struct pgraph_texture_possibly_dirty_struct {
    hwaddr addr, end;
};

static void mark_textures_possibly_dirty_visitor(Lru *lru, LruNode *node,
                                                 void *opaque)
{
    struct pgraph_texture_possibly_dirty_struct *test = opaque;

    TextureBinding *tnode = container_of(node, TextureBinding, node);
    if (tnode->possibly_dirty) {
        return;
    }

    uintptr_t k_tex_addr = tnode->key.texture_vram_offset;
    uintptr_t k_tex_end = k_tex_addr + tnode->key.texture_length - 1;
    bool overlapping = !(test->addr > k_tex_end || k_tex_addr > test->end);

    if (tnode->key.palette_length > 0) {
        uintptr_t k_pal_addr = tnode->key.palette_vram_offset;
        uintptr_t k_pal_end = k_pal_addr + tnode->key.palette_length - 1;
        overlapping |= !(test->addr > k_pal_end || k_pal_addr > test->end);
    }

    tnode->possibly_dirty |= overlapping;
}

void pgraph_wgpu_mark_textures_possibly_dirty(NV2AState *d, hwaddr addr,
                                              hwaddr size)
{
    hwaddr end = TARGET_PAGE_ALIGN(addr + size) - 1;
    addr &= TARGET_PAGE_MASK;
    end = MIN(end, memory_region_size(d->vram));

    struct pgraph_texture_possibly_dirty_struct test = {
        .addr = addr,
        .end = end,
    };

    lru_visit_active(&d->pgraph.wgpu_renderer_state->tex.texture_cache,
                     mark_textures_possibly_dirty_visitor, &test);
}

static bool check_texture_dirty(NV2AState *d, hwaddr addr, hwaddr size)
{
    hwaddr vram_size = memory_region_size(d->vram);
    hwaddr end = MIN(TARGET_PAGE_ALIGN(addr + size), vram_size);
    addr &= TARGET_PAGE_MASK;
    /* vk asserts end < size: off by one for textures ending at the last
     * VRAM page, and games do place textures there */
    if (addr >= end) {
        return false;
    }
    return memory_region_test_and_clear_dirty(d->vram, addr, end - addr,
                                              DIRTY_MEMORY_NV2A_TEX);
}

// Check if any of the pages spanned by the a texture are dirty.
static bool check_texture_possibly_dirty(NV2AState *d,
                                         hwaddr texture_vram_offset,
                                         unsigned int length,
                                         hwaddr palette_vram_offset,
                                         unsigned int palette_length)
{
    bool possibly_dirty = false;
    if (check_texture_dirty(d, texture_vram_offset, length)) {
        possibly_dirty = true;
        pgraph_wgpu_mark_textures_possibly_dirty(d, texture_vram_offset,
                                                 length);
    }
    if (palette_length &&
        check_texture_dirty(d, palette_vram_offset, palette_length)) {
        possibly_dirty = true;
        pgraph_wgpu_mark_textures_possibly_dirty(d, palette_vram_offset,
                                                 palette_length);
    }
    return possibly_dirty;
}

static void release_texture_image(TextureBinding *binding)
{
    if (binding->view) {
        wgpuTextureViewRelease(binding->view);
        binding->view = NULL;
    }
    if (binding->texture) {
        /* No wgpuTextureDestroy: commands already recorded in the open
         * encoder may still sample it; the last reference frees it. */
        wgpuTextureRelease(binding->texture);
        binding->texture = NULL;
    }
}

/*
 * (Re)create the WGPUTexture and view of @binding. Any previous texture is
 * released (not destroyed), see the header comment on queue write ordering.
 */
static void create_texture_image(PGRAPHWgpuState *r, TextureBinding *binding,
                                 WGPUTextureFormat format, unsigned int width,
                                 unsigned int height, unsigned int depth,
                                 unsigned int mip_levels,
                                 WGPUTextureUsage extra_usage)
{
    TextureShape *state = &binding->key.state;

    release_texture_image(binding);

    g_autofree gchar *label = g_strdup_printf(
        "Texture %" HWADDR_PRIx "h fmt:%02xh %dx%dx%d lvls:%d",
        binding->key.texture_vram_offset, state->color_format, state->width,
        state->height, state->depth, state->levels);

    bool is_3d = state->dimensionality == 3;
    unsigned int array_layers = state->cubemap ? 6 : 1;

    WGPUTextureDescriptor desc = {
        .label = { label, WGPU_STRLEN },
        .usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst |
                 extra_usage,
        .dimension = is_3d ? WGPUTextureDimension_3D : WGPUTextureDimension_2D,
        .size = { width, height, is_3d ? depth : array_layers },
        .format = format,
        .mipLevelCount = mip_levels,
        .sampleCount = 1,
    };
    binding->texture = wgpuDeviceCreateTexture(r->device, &desc);

    WGPUTextureViewDimension view_dimension =
        state->cubemap ? WGPUTextureViewDimension_Cube :
        is_3d          ? WGPUTextureViewDimension_3D :
                         WGPUTextureViewDimension_2D;

    WGPUTextureViewDescriptor view_desc = {
        .label = { label, WGPU_STRLEN },
        .format = format,
        .dimension = view_dimension,
        .baseMipLevel = 0,
        .mipLevelCount = mip_levels,
        .baseArrayLayer = 0,
        .arrayLayerCount = is_3d ? 1 : array_layers,
        .aspect = WGPUTextureAspect_All,
    };
    binding->view = wgpuTextureCreateView(binding->texture, &view_desc);

    binding->format = format;
    binding->view_dimension = view_dimension;
    binding->sample_type = sample_type_for_format(r, format);
    binding->from_surface = extra_usage != WGPUTextureUsage_None;
    binding->width = width;
    binding->height = height;
    binding->depth = is_3d ? depth : 1;
    binding->mip_levels = mip_levels;
    binding->array_layers = is_3d ? 1 : array_layers;
}

// FIXME: Make sure we update sampler when data matches. Should we add filtering
// options to the textureshape?
static void upload_texture_image(PGRAPHState *pg, int texture_idx,
                                 TextureBinding *binding)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;
    TextureShape *state = &binding->key.state;
    BasicColorFormatInfo f_basic =
        kelvin_color_format_info_map[state->color_format];

    nv2a_profile_inc_counter(NV2A_PROF_TEX_UPLOAD);

    g_autofree TextureLayout *layout = get_texture_layout(pg, texture_idx);
    const int num_layers = state->cubemap ? 6 : 1;
    const int mip_levels = f_basic.linear ? 1 : state->levels;

    create_texture_image(r, binding, layout->format, layout->width,
                         layout->height, layout->depth, mip_levels,
                         WGPUTextureUsage_None);

    for (int layer_idx = 0; layer_idx < num_layers; layer_idx++) {
        TextureLayer *layer = &layout->layers[layer_idx];
        for (int level_idx = 0; level_idx < mip_levels; level_idx++) {
            TextureLevel *level = &layer->levels[level_idx];
            assert(level->decoded_size);

            WGPUTexelCopyTextureInfo dst = {
                .texture = binding->texture,
                .mipLevel = level_idx,
                .origin = { 0, 0, layer_idx },
                .aspect = WGPUTextureAspect_All,
            };
            WGPUTexelCopyBufferLayout data_layout = {
                .offset = 0,
                .bytesPerRow = level->bytes_per_row,
                .rowsPerImage = level->rows_per_image,
            };
            WGPUExtent3D size = { level->width, level->height, level->depth };
            XSTAT_INC(n_tex_upload);
            XSTAT_ADD(b_tex_upload, level->decoded_size);
            wgpuQueueWriteTexture(r->queue, &dst, level->decoded_data,
                                  level->decoded_size, &data_layout, &size);
        }
    }

    // Release decoded texture data
    for (int layer_idx = 0; layer_idx < num_layers; layer_idx++) {
        TextureLayer *layer = &layout->layers[layer_idx];
        for (int level_idx = 0; level_idx < mip_levels; level_idx++) {
            g_free(layer->levels[level_idx].decoded_data);
        }
    }
}

/*
 * Render-to-texture (vk: copy_surface_to_texture / copy_zeta_surface_to_texture)
 *
 * vk copies the surface image into the texture image. Here texture formats
 * hold the *sampled* value of the NV2A texture format, so the surface is
 * converted on the GPU, recorded on the open encoder (ordered after the draws
 * that rendered the surface and before later draws sampling the texture):
 *  - S2T_COLOR: render pass sampling the color surface (textureLoad) into a
 *    BGRA8/RGBA8 texture, optionally forcing alpha to 1 (X formats).
 *  - S2T_DEPTH16: render pass reading a Z16 surface's depth into the
 *    RGBA32Float (depth, 0, is_float, 0) layout of the Y16 depth formats.
 *  - S2T_Z24S8: the surface module packs depth+stencil into Z24S8 words
 *    (pgraph_wgpu_pack_depth_stencil); a compute pass stores them into the
 *    R32Uint texture of the X8_Y24 formats (a buffer->texture copy would
 *    need 256 byte aligned rows).
 *  - S2T_Z24S8_PACKED_COLOR: the same packed words, interpreted as the raw
 *    bytes of a linear A8R8G8B8 or B8G8R8A8 texture by a render pass. This
 *    retains both the depth quantization and stencil byte of the VRAM path.
 * Every other surface/texture combination takes the VRAM path (surface is
 * downloaded if dirty, texture decoded from VRAM).
 */
typedef enum SurfaceToTextureMode {
    S2T_NONE,
    S2T_COLOR,
    S2T_DEPTH16,
    S2T_Z24S8,
    S2T_Z24S8_PACKED_COLOR,
} SurfaceToTextureMode;

#define S2T_WGSL_VS \
    "@vertex fn vs(@builtin(vertex_index) i: u32) -> @builtin(position) vec4f {\n" \
    "    var p = array(vec2f(-1.0, -1.0), vec2f(3.0, -1.0), vec2f(-1.0, 3.0));\n" \
    "    return vec4f(p[i], 0.0, 1.0);\n" \
    "}\n"

static const char s2t_color_wgsl[] =
    "@group(0) @binding(0) var src: texture_2d<f32>;\n"
    S2T_WGSL_VS
    "fn load(pos: vec4f) -> vec4f {\n"
    "    let dim = vec2i(textureDimensions(src)) - vec2i(1, 1);\n"
    "    return textureLoad(src, min(vec2i(pos.xy), dim), 0);\n"
    "}\n"
    "@fragment fn fs_copy(@builtin(position) pos: vec4f) -> @location(0) vec4f {\n"
    "    return load(pos);\n"
    "}\n"
    "@fragment fn fs_opaque(@builtin(position) pos: vec4f) -> @location(0) vec4f {\n"
    "    return vec4f(load(pos).rgb, 1.0);\n"
    "}\n";

/* (depth, 0, is_float, 0): see kelvin_color_format_wgpu_map depth notes */
static const char s2t_depth_wgsl[] =
    "@group(0) @binding(0) var src: texture_depth_2d;\n"
    S2T_WGSL_VS
    "fn load(pos: vec4f) -> f32 {\n"
    "    let dim = vec2i(textureDimensions(src)) - vec2i(1, 1);\n"
    "    return textureLoad(src, min(vec2i(pos.xy), dim), 0);\n"
    "}\n"
    "@fragment fn fs_depth16_fixed(@builtin(position) pos: vec4f) -> @location(0) vec4f {\n"
    "    return vec4f(load(pos), 0.0, 0.0, 0.0);\n"
    "}\n"
    "@fragment fn fs_depth16_float(@builtin(position) pos: vec4f) -> @location(0) vec4f {\n"
    "    return vec4f(load(pos), 0.0, 1.0, 0.0);\n"
    "}\n";

static const char s2t_z24s8_wgsl[] =
    "@group(0) @binding(0) var<storage, read> src: array<u32>;\n"
    "@group(0) @binding(1) var dst: texture_storage_2d<r32uint, write>;\n"
    "@compute @workgroup_size(8, 8)\n"
    "fn cs_z24s8(@builtin(global_invocation_id) id: vec3u) {\n"
    "    let dim = textureDimensions(dst);\n"
    "    if (id.x >= dim.x || id.y >= dim.y) { return; }\n"
    "    textureStore(dst, vec2i(id.xy), vec4u(src[id.y * dim.x + id.x], 0u, 0u, 0u));\n"
    "}\n";

/*
 * unpack4x8unorm exposes the little-endian guest bytes b0..b3. Match the
 * sampled RGBA of convert_texels(): A8R8G8B8 is uploaded verbatim to BGRA8,
 * whereas B8G8R8A8 is converted to RGBA8 with CONV_RGBA8_GBAR.
 * The depth view supplies the packed row width, including surface scaling;
 * no mutable uniform buffer / queue write is needed between conversions.
 */
static const char s2t_packed_color_wgsl[] =
    "@group(0) @binding(0) var<storage, read> src: array<u32>;\n"
    "@group(0) @binding(1) var extent_src: texture_depth_2d;\n"
    S2T_WGSL_VS
    "fn load(pos: vec4f) -> vec4f {\n"
    "    let dim = textureDimensions(extent_src);\n"
    "    let c = vec2u(pos.xy);\n"
    "    return unpack4x8unorm(src[c.y * dim.x + c.x]);\n"
    "}\n"
    "@fragment fn fs_a8r8g8b8(@builtin(position) pos: vec4f) -> @location(0) vec4f {\n"
    "    return load(pos).bgra;\n"
    "}\n"
    "@fragment fn fs_b8g8r8a8(@builtin(position) pos: vec4f) -> @location(0) vec4f {\n"
    "    return load(pos).gbar;\n"
    "}\n";

static WGPUPipelineLayout create_pipeline_layout(PGRAPHWgpuState *r,
                                                 WGPUBindGroupLayout *bgl,
                                                 const WGPUBindGroupLayoutEntry
                                                     *entries,
                                                 size_t entry_count)
{
    *bgl = wgpuDeviceCreateBindGroupLayout(
        r->device, &(WGPUBindGroupLayoutDescriptor){ .entryCount = entry_count,
                                                     .entries = entries });
    return wgpuDeviceCreatePipelineLayout(
        r->device, &(WGPUPipelineLayoutDescriptor){ .bindGroupLayoutCount = 1,
                                                    .bindGroupLayouts = bgl });
}

static void init_surface_to_texture(PGRAPHWgpuState *r)
{
    PGRAPHWgpuTextureState *t = &r->tex;

    t->s2t_color_module = pgraph_wgpu_create_wgsl_module(
        r, "surface-to-texture color", s2t_color_wgsl);
    t->s2t_depth_module = pgraph_wgpu_create_wgsl_module(
        r, "surface-to-texture depth", s2t_depth_wgsl);
    t->s2t_z24s8_module = pgraph_wgpu_create_wgsl_module(
        r, "surface-to-texture z24s8", s2t_z24s8_wgsl);
    t->s2t_packed_color_module = pgraph_wgpu_create_wgsl_module(
        r, "surface-to-texture packed color", s2t_packed_color_wgsl);

    WGPUBindGroupLayoutEntry color_entry = {
        .binding = 0,
        .visibility = WGPUShaderStage_Fragment,
        .texture = { .sampleType = WGPUTextureSampleType_UnfilterableFloat,
                     .viewDimension = WGPUTextureViewDimension_2D },
    };
    t->s2t_color_layout =
        create_pipeline_layout(r, &t->s2t_color_bgl, &color_entry, 1);

    WGPUBindGroupLayoutEntry depth_entry = {
        .binding = 0,
        .visibility = WGPUShaderStage_Fragment,
        .texture = { .sampleType = WGPUTextureSampleType_Depth,
                     .viewDimension = WGPUTextureViewDimension_2D },
    };
    t->s2t_depth_layout =
        create_pipeline_layout(r, &t->s2t_depth_bgl, &depth_entry, 1);

    WGPUBindGroupLayoutEntry z24s8_entries[2] = {
        { .binding = 0,
          .visibility = WGPUShaderStage_Compute,
          .buffer = { .type = WGPUBufferBindingType_ReadOnlyStorage } },
        { .binding = 1,
          .visibility = WGPUShaderStage_Compute,
          .storageTexture = { .access = WGPUStorageTextureAccess_WriteOnly,
                              .format = WGPUTextureFormat_R32Uint,
                              .viewDimension = WGPUTextureViewDimension_2D } },
    };
    t->s2t_z24s8_layout =
        create_pipeline_layout(r, &t->s2t_z24s8_bgl, z24s8_entries, 2);

    WGPUBindGroupLayoutEntry packed_color_entries[2] = {
        { .binding = 0,
          .visibility = WGPUShaderStage_Fragment,
          .buffer = { .type = WGPUBufferBindingType_ReadOnlyStorage } },
        { .binding = 1,
          .visibility = WGPUShaderStage_Fragment,
          .texture = { .sampleType = WGPUTextureSampleType_Depth,
                       .viewDimension = WGPUTextureViewDimension_2D } },
    };
    t->s2t_packed_color_layout = create_pipeline_layout(
        r, &t->s2t_packed_color_bgl, packed_color_entries, 2);
}

static WGPURenderPipeline create_s2t_render_pipeline(PGRAPHWgpuState *r,
                                                     WGPUShaderModule module,
                                                     WGPUPipelineLayout layout,
                                                     WGPUTextureFormat format,
                                                     const char *entry_point)
{
    WGPUColorTargetState target = {
        .format = format,
        .writeMask = WGPUColorWriteMask_All,
    };
    WGPUFragmentState frag = {
        .module = module,
        .entryPoint = { entry_point, WGPU_STRLEN },
        .targetCount = 1,
        .targets = &target,
    };
    WGPURenderPipelineDescriptor desc = {
        .label = { "surface-to-texture", WGPU_STRLEN },
        .layout = layout,
        .vertex = { .module = module,
                    .entryPoint = { "vs", WGPU_STRLEN } },
        .primitive = { .topology = WGPUPrimitiveTopology_TriangleList },
        .multisample = { .count = 1, .mask = ~0u },
        .fragment = &frag,
    };
    return wgpuDeviceCreateRenderPipeline(r->device, &desc);
}

static WGPURenderPipeline get_s2t_color_pipeline(PGRAPHWgpuState *r,
                                                 WGPUTextureFormat format,
                                                 bool opaque)
{
    assert(format == WGPUTextureFormat_BGRA8Unorm ||
           format == WGPUTextureFormat_RGBA8Unorm);
    int fmt_idx = format == WGPUTextureFormat_BGRA8Unorm ? 0 : 1;

    WGPURenderPipeline *p = &r->tex.s2t_color_pipelines[fmt_idx][opaque];
    if (!*p) {
        *p = create_s2t_render_pipeline(r, r->tex.s2t_color_module,
                                        r->tex.s2t_color_layout, format,
                                        opaque ? "fs_opaque" : "fs_copy");
    }
    return *p;
}

static WGPURenderPipeline get_s2t_depth_pipeline(PGRAPHWgpuState *r,
                                                 bool is_float)
{
    WGPURenderPipeline *p = &r->tex.s2t_depth_pipelines[is_float];
    if (!*p) {
        *p = create_s2t_render_pipeline(
            r, r->tex.s2t_depth_module, r->tex.s2t_depth_layout, WGPUTextureFormat_RGBA32Float,
            is_float ? "fs_depth16_float" : "fs_depth16_fixed");
    }
    return *p;
}

static WGPUComputePipeline get_s2t_z24s8_pipeline(PGRAPHWgpuState *r)
{
    PGRAPHWgpuTextureState *t = &r->tex;

    if (!t->s2t_z24s8_pipeline) {
        WGPUComputePipelineDescriptor desc = {
            .label = { "surface-to-texture z24s8", WGPU_STRLEN },
            .layout = t->s2t_z24s8_layout,
            .compute = { .module = t->s2t_z24s8_module,
                         .entryPoint = { "cs_z24s8", WGPU_STRLEN } },
        };
        t->s2t_z24s8_pipeline =
            wgpuDeviceCreateComputePipeline(r->device, &desc);
    }
    return t->s2t_z24s8_pipeline;
}

static WGPURenderPipeline get_s2t_packed_color_pipeline(PGRAPHWgpuState *r,
                                                       WGPUTextureFormat format)
{
    assert(format == WGPUTextureFormat_BGRA8Unorm ||
           format == WGPUTextureFormat_RGBA8Unorm);
    int fmt_idx = format == WGPUTextureFormat_BGRA8Unorm ? 0 : 1;
    WGPURenderPipeline *p = &r->tex.s2t_packed_color_pipelines[fmt_idx];

    if (!*p) {
        *p = create_s2t_render_pipeline(
            r, r->tex.s2t_packed_color_module, r->tex.s2t_packed_color_layout,
            format, fmt_idx == 0 ? "fs_a8r8g8b8" : "fs_b8g8r8a8");
    }
    return *p;
}

static void finalize_surface_to_texture(PGRAPHWgpuState *r)
{
    PGRAPHWgpuTextureState *t = &r->tex;

    for (int i = 0; i < 2; i++) {
        for (int j = 0; j < 2; j++) {
            if (t->s2t_color_pipelines[i][j]) {
                wgpuRenderPipelineRelease(t->s2t_color_pipelines[i][j]);
                t->s2t_color_pipelines[i][j] = NULL;
            }
        }
        if (t->s2t_depth_pipelines[i]) {
            wgpuRenderPipelineRelease(t->s2t_depth_pipelines[i]);
            t->s2t_depth_pipelines[i] = NULL;
        }
        if (t->s2t_packed_color_pipelines[i]) {
            wgpuRenderPipelineRelease(t->s2t_packed_color_pipelines[i]);
            t->s2t_packed_color_pipelines[i] = NULL;
        }
    }
    if (t->s2t_z24s8_pipeline) {
        wgpuComputePipelineRelease(t->s2t_z24s8_pipeline);
        t->s2t_z24s8_pipeline = NULL;
    }

    WGPUPipelineLayout *layouts[] = { &t->s2t_color_layout,
                                      &t->s2t_depth_layout,
                                      &t->s2t_z24s8_layout,
                                      &t->s2t_packed_color_layout };
    WGPUBindGroupLayout *bgls[] = { &t->s2t_color_bgl, &t->s2t_depth_bgl,
                                    &t->s2t_z24s8_bgl,
                                    &t->s2t_packed_color_bgl };
    for (int i = 0; i < ARRAY_SIZE(layouts); i++) {
        if (*layouts[i]) {
            wgpuPipelineLayoutRelease(*layouts[i]);
            *layouts[i] = NULL;
        }
        if (*bgls[i]) {
            wgpuBindGroupLayoutRelease(*bgls[i]);
            *bgls[i] = NULL;
        }
    }
    WGPUShaderModule *modules[] = { &t->s2t_color_module,
                                    &t->s2t_depth_module,
                                    &t->s2t_z24s8_module,
                                    &t->s2t_packed_color_module };
    for (int i = 0; i < ARRAY_SIZE(modules); i++) {
        if (*modules[i]) {
            wgpuShaderModuleRelease(*modules[i]);
            *modules[i] = NULL;
        }
    }
}

static bool is_y16_depth_format(unsigned int color_format)
{
    return color_format == NV097_SET_TEXTURE_FORMAT_COLOR_SZ_DEPTH_Y16_FIXED ||
           color_format ==
               NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_DEPTH_Y16_FIXED ||
           color_format ==
               NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_DEPTH_Y16_FLOAT;
}

static bool is_x8y24_depth_format(unsigned int color_format)
{
    return color_format ==
               NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_DEPTH_X8_Y24_FIXED ||
           color_format ==
               NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_DEPTH_X8_Y24_FLOAT;
}

static SurfaceToTextureMode
check_surface_to_texture_compatiblity(const SurfaceBinding *surface,
                                      const TextureShape *shape)
{
    if ((!surface->swizzle && surface->pitch != shape->pitch) ||
        surface->width != shape->width ||
        surface->height != shape->height ||
        shape->cubemap ||
        shape->levels > 1) {
        return S2T_NONE;
    }

    /* vk: same texel size (raw copy); here also the same kind of data */
    if (surface->fmt.bytes_per_pixel !=
        kelvin_color_format_info_map[shape->color_format].bytes_per_pixel) {
        return S2T_NONE;
    }

    if (!surface->color) {
        if (surface->host_fmt.conv == WGPU_SURFACE_CONV_Z24S8 &&
            surface->depth_view && surface->stencil_view) {
            if (is_x8y24_depth_format(shape->color_format)) {
                return S2T_Z24S8;
            }
            /*
             * Raw Z24S8 bytes sampled as color (not normalized depth).
             * Start with identical linear 2D layouts only. In particular,
             * pitch < row bytes aliases rows on the VRAM path, and a
             * swizzled surface cannot be reinterpreted as linear texels.
             */
            if (!surface->swizzle && shape->dimensionality == 2 &&
                shape->depth == 1 && shape->width && shape->height &&
                shape->pitch >= (uint64_t)shape->width * 4 &&
                (shape->color_format ==
                     NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8R8G8B8 ||
                 shape->color_format ==
                     NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_B8G8R8A8)) {
                return S2T_Z24S8_PACKED_COLOR;
            }
        }
        if (is_y16_depth_format(shape->color_format) &&
            surface->host_fmt.depth && !surface->host_fmt.stencil &&
            (surface->depth_view || surface->view)) {
            return S2T_DEPTH16;
        }
        return S2T_NONE;
    }

    /*
     * Color: the blit copies sampled color, so only texture formats whose
     * sampled color is the surface's are handled here (vk's raw texel copy
     * also reinterprets bits between formats of equal size; those cases take
     * the VRAM path).
     */
    if (!surface->view ||
        !kelvin_color_format_wgpu_map[shape->color_format].from_surface) {
        return S2T_NONE;
    }
    return S2T_COLOR;
}

// FIXME: Should be able to skip the copy and sample the original surface image
static void copy_surface_to_texture(PGRAPHState *pg, SurfaceBinding *surface,
                                    TextureBinding *texture,
                                    SurfaceToTextureMode mode)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;
    TextureShape *state = &texture->key.state;
    const WgpuColorFormatInfo *wf =
        &kelvin_color_format_wgpu_map[state->color_format];

    assert(mode != S2T_NONE);

    nv2a_profile_inc_counter(NV2A_PROF_SURF_TO_TEX);

    trace_nv2a_pgraph_surface_render_to_texture(
        surface->vram_addr, surface->width, surface->height);

    unsigned int scaled_width = surface->width,
                 scaled_height = surface->height;
    pgraph_apply_scaling_factor(pg, &scaled_width, &scaled_height);

    WGPUTextureUsage usage = mode == S2T_Z24S8 ?
                                 WGPUTextureUsage_StorageBinding :
                                 WGPUTextureUsage_RenderAttachment;

    if (!texture->texture || !texture->from_surface ||
        texture->format != wf->format || texture->width != scaled_width ||
        texture->height != scaled_height || texture->mip_levels != 1) {
        create_texture_image(r, texture, wf->format, scaled_width,
                             scaled_height, 1, 1, usage);
    }

    WGPUCommandEncoder enc = pgraph_wgpu_begin_nondraw_commands(pg);

    if (mode == S2T_Z24S8) {
        pgraph_wgpu_pack_depth_stencil(pg, surface, enc);

        WGPUBindGroupEntry entries[2] = {
            { .binding = 0, .buffer = r->surf.compute.pack_dst, .offset = 0,
              .size = (uint64_t)scaled_width * scaled_height * 4 },
            { .binding = 1, .textureView = texture->view },
        };
        WGPUBindGroup bind_group = wgpuDeviceCreateBindGroup(
            r->device, &(WGPUBindGroupDescriptor){
                           .layout = r->tex.s2t_z24s8_bgl,
                           .entryCount = 2,
                           .entries = entries });

        WGPUComputePassEncoder pass = wgpuCommandEncoderBeginComputePass(
            enc, &(WGPUComputePassDescriptor){
                     .label = { "surface-to-texture z24s8", WGPU_STRLEN } });
        wgpuComputePassEncoderSetPipeline(pass, get_s2t_z24s8_pipeline(r));
        wgpuComputePassEncoderSetBindGroup(pass, 0, bind_group, 0, NULL);
        wgpuComputePassEncoderDispatchWorkgroups(pass,
                                                 DIV_ROUND_UP(scaled_width, 8),
                                                 DIV_ROUND_UP(scaled_height, 8),
                                                 1);
        wgpuComputePassEncoderEnd(pass);
        wgpuComputePassEncoderRelease(pass);
        wgpuBindGroupRelease(bind_group);
    } else {
        WGPUBindGroupLayout bgl;
        WGPURenderPipeline pipeline;
        WGPUBindGroupEntry entries[2] = { { .binding = 0 } };
        size_t entry_count = 1;

        if (mode == S2T_COLOR) {
            bgl = r->tex.s2t_color_bgl;
            pipeline = get_s2t_color_pipeline(r, wf->format,
                                              wf->from_surface_opaque);
            entries[0].textureView = surface->view;
        } else if (mode == S2T_Z24S8_PACKED_COLOR) {
            /* Pack and decode on the same encoder; never map pack_dst. */
            pgraph_wgpu_pack_depth_stencil(pg, surface, enc);
            bgl = r->tex.s2t_packed_color_bgl;
            pipeline = get_s2t_packed_color_pipeline(r, wf->format);
            entries[0].buffer = r->surf.compute.pack_dst;
            entries[0].size = (uint64_t)scaled_width * scaled_height * 4;
            entries[1] = (WGPUBindGroupEntry){
                .binding = 1, .textureView = surface->depth_view,
            };
            entry_count = 2;
#ifdef EMSCRIPTEN
            /* A GPU-only hit, not an actual surface download. */
            char key[128];
            snprintf(key, sizeof(key),
                     "dl:gpu-zeta-s2t tex:fmt%u %ux%u p%u",
                     state->color_format, state->width, state->height,
                     state->pitch);
            xemu_wasm_count(g_intern_string(key));
#endif
        } else {
            assert(mode == S2T_DEPTH16);
            bgl = r->tex.s2t_depth_bgl;
            pipeline = get_s2t_depth_pipeline(
                r, wf->conv == CONV_DEPTH16_FLOAT);
            entries[0].textureView = surface->depth_view ? surface->depth_view :
                                                         surface->view;
        }

        WGPUBindGroup bind_group = wgpuDeviceCreateBindGroup(
            r->device, &(WGPUBindGroupDescriptor){ .layout = bgl,
                                                   .entryCount = entry_count,
                                                   .entries = entries });

        WGPURenderPassColorAttachment ca = {
            .view = texture->view,
            .depthSlice = WGPU_DEPTH_SLICE_UNDEFINED,
            .loadOp = WGPULoadOp_Clear,
            .storeOp = WGPUStoreOp_Store,
            .clearValue = { 0, 0, 0, 0 },
        };
        WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(
            enc, &(WGPURenderPassDescriptor){
                     .label = { "surface-to-texture", WGPU_STRLEN },
                     .colorAttachmentCount = 1,
                     .colorAttachments = &ca });
        wgpuRenderPassEncoderSetPipeline(pass, pipeline);
        wgpuRenderPassEncoderSetBindGroup(pass, 0, bind_group, 0, NULL);
        wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
        wgpuRenderPassEncoderEnd(pass);
        wgpuRenderPassEncoderRelease(pass);
        wgpuBindGroupRelease(bind_group);
    }

    pgraph_wgpu_end_nondraw_commands(pg, enc);

    texture->draw_time = surface->draw_time;
}

static WGPUSampler create_sampler(PGRAPHWgpuState *r, const TextureKey *key,
                                  WGPUTextureFormat format,
                                  unsigned int mip_levels)
{
    const TextureShape *state = &key->state;
    BasicColorFormatInfo f_basic =
        kelvin_color_format_info_map[state->color_format];
    uint32_t filter = key->filter;
    uint32_t address = key->address;

    if (filter & NV_PGRAPH_TEXFILTER0_ASIGNED)
        NV2A_UNIMPLEMENTED("NV_PGRAPH_TEXFILTER0_ASIGNED");
    if (filter & NV_PGRAPH_TEXFILTER0_RSIGNED)
        NV2A_UNIMPLEMENTED("NV_PGRAPH_TEXFILTER0_RSIGNED");
    if (filter & NV_PGRAPH_TEXFILTER0_GSIGNED)
        NV2A_UNIMPLEMENTED("NV_PGRAPH_TEXFILTER0_GSIGNED");
    if (filter & NV_PGRAPH_TEXFILTER0_BSIGNED)
        NV2A_UNIMPLEMENTED("NV_PGRAPH_TEXFILTER0_BSIGNED");

    unsigned int mag_filter = GET_MASK(filter, NV_PGRAPH_TEXFILTER0_MAG);
    assert(mag_filter < ARRAY_SIZE(pgraph_texture_min_filter_wgpu_map));

    unsigned int min_filter = GET_MASK(filter, NV_PGRAPH_TEXFILTER0_MIN);
    assert(min_filter < ARRAY_SIZE(pgraph_texture_min_filter_wgpu_map));

    bool filterable = is_linear_filter_supported_for_format(r, format);

    WGPUFilterMode wgpu_min_filter, wgpu_mag_filter;
    if (filterable) {
        wgpu_mag_filter = pgraph_texture_min_filter_wgpu_map[mag_filter];
        wgpu_min_filter = pgraph_texture_min_filter_wgpu_map[min_filter];
    } else {
        wgpu_mag_filter = wgpu_min_filter = WGPUFilterMode_Nearest;
    }

    bool mipmap_en =
        !f_basic.linear &&
        !(min_filter == NV_PGRAPH_TEXFILTER0_MIN_BOX_LOD0 ||
          min_filter == NV_PGRAPH_TEXFILTER0_MIN_TENT_LOD0 ||
          min_filter == NV_PGRAPH_TEXFILTER0_MIN_CONVOLUTION_2D_LOD0);

    bool mipmap_nearest =
        !filterable || f_basic.linear || mip_levels == 1 ||
        min_filter == NV_PGRAPH_TEXFILTER0_MIN_BOX_NEARESTLOD ||
        min_filter == NV_PGRAPH_TEXFILTER0_MIN_TENT_NEARESTLOD;

    /* FIXME: WebGPU samplers have no LOD bias (NV_PGRAPH_TEXFILTER0_
     * MIPMAP_LOD_BIAS); it would have to be applied in the shader. */

    float min_lod = 0.0f, max_lod = 0.0f;
    if (mipmap_en) {
        min_lod = MIN(state->min_mipmap_level, state->levels - 1);
        max_lod = MIN(state->max_mipmap_level, state->levels - 1);
        min_lod = MIN(min_lod, max_lod);
    }

    WGPUMipmapFilterMode mipmap_filter = mipmap_nearest ?
                                             WGPUMipmapFilterMode_Nearest :
                                             WGPUMipmapFilterMode_Linear;

    /* WebGPU only allows anisotropy with linear mag/min/mipmap filtering
     * (vk enables it regardless) and clamps it to 16. */
    uint16_t max_anisotropy = 1;
    if (wgpu_mag_filter == WGPUFilterMode_Linear &&
        wgpu_min_filter == WGPUFilterMode_Linear &&
        mipmap_filter == WGPUMipmapFilterMode_Linear) {
        max_anisotropy = MIN(key->max_anisotropy, 16);
    }

    /* FIXME: CLAMP_TO_BORDER / border color: not supported by WebGPU,
     * approximated with ClampToEdge (key->border_color unused). */
    WGPUSamplerDescriptor desc = {
        .label = { "texture", WGPU_STRLEN },
        .addressModeU = lookup_texture_address_mode(
            GET_MASK(address, NV_PGRAPH_TEXADDRESS0_ADDRU)),
        .addressModeV = lookup_texture_address_mode(
            GET_MASK(address, NV_PGRAPH_TEXADDRESS0_ADDRV)),
        .addressModeW = (state->dimensionality > 2) ?
                            lookup_texture_address_mode(
                                GET_MASK(address, NV_PGRAPH_TEXADDRESS0_ADDRP)) :
                            WGPUAddressMode_Repeat,
        .magFilter = wgpu_mag_filter,
        .minFilter = wgpu_min_filter,
        .mipmapFilter = mipmap_filter,
        .lodMinClamp = min_lod,
        .lodMaxClamp = max_lod,
        .compare = WGPUCompareFunction_Undefined,
        .maxAnisotropy = max_anisotropy,
    };

    return wgpuDeviceCreateSampler(r->device, &desc);
}

static void create_dummy_texture(PGRAPHWgpuState *r, TextureBinding *binding,
                                 WGPUTextureViewDimension view_dimension)
{
    unsigned int size = view_dimension == WGPUTextureViewDimension_2D ? 16 : 1;
    unsigned int layers = view_dimension == WGPUTextureViewDimension_Cube ? 6 :
                                                                            1;
    bool is_3d = view_dimension == WGPUTextureViewDimension_3D;

    *binding = (TextureBinding){
        .key.scale = 1.0,
        .format = WGPUTextureFormat_RGBA8Unorm,
        .view_dimension = view_dimension,
        .sample_type = WGPUTextureSampleType_Float,
        .sampler_type = WGPUSamplerBindingType_Filtering,
        .width = size,
        .height = size,
        .depth = 1,
        .mip_levels = 1,
        .array_layers = layers,
    };

    WGPUTextureDescriptor desc = {
        .label = { "dummy texture", WGPU_STRLEN },
        .usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst,
        .dimension = is_3d ? WGPUTextureDimension_3D : WGPUTextureDimension_2D,
        .size = { size, size, layers },
        .format = WGPUTextureFormat_RGBA8Unorm,
        .mipLevelCount = 1,
        .sampleCount = 1,
    };
    binding->texture = wgpuDeviceCreateTexture(r->device, &desc);

    WGPUTextureViewDescriptor view_desc = {
        .label = { "dummy texture", WGPU_STRLEN },
        .format = WGPUTextureFormat_RGBA8Unorm,
        .dimension = view_dimension,
        .baseMipLevel = 0,
        .mipLevelCount = 1,
        .baseArrayLayer = 0,
        .arrayLayerCount = layers,
        .aspect = WGPUTextureAspect_All,
    };
    binding->view = wgpuTextureCreateView(binding->texture, &view_desc);

    binding->sampler = wgpuDeviceCreateSampler(
        r->device, &(WGPUSamplerDescriptor){
                       .label = { "dummy texture", WGPU_STRLEN },
                       .addressModeU = WGPUAddressMode_Repeat,
                       .addressModeV = WGPUAddressMode_Repeat,
                       .addressModeW = WGPUAddressMode_Repeat,
                       .magFilter = WGPUFilterMode_Nearest,
                       .minFilter = WGPUFilterMode_Nearest,
                       .mipmapFilter = WGPUMipmapFilterMode_Nearest,
                       .lodMinClamp = 0.0f,
                       .lodMaxClamp = 32.0f,
                       .maxAnisotropy = 1 });

    /* vk: R8 filled with 0xff, swizzled RRRR -> opaque white */
    size_t data_size = (size_t)size * size * layers * 4;
    g_autofree uint8_t *data = g_malloc(data_size);
    memset(data, 0xff, data_size);

    WGPUTexelCopyTextureInfo dst = {
        .texture = binding->texture,
        .aspect = WGPUTextureAspect_All,
    };
    WGPUTexelCopyBufferLayout layout = {
        .bytesPerRow = size * 4,
        .rowsPerImage = size,
    };
    WGPUExtent3D extent = { size, size, layers };
    wgpuQueueWriteTexture(r->queue, &dst, data, data_size, &layout, &extent);
}

static void create_texture(PGRAPHState *pg, int texture_idx)
{
    NV2AState *d = container_of(pg, NV2AState, pgraph);
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;
    TextureShape state = pgraph_get_texture_shape(pg, texture_idx);

    const hwaddr texture_vram_offset =
        pgraph_get_texture_phys_addr(pg, texture_idx);
    size_t texture_length = pgraph_get_texture_length(pg, &state);
    hwaddr texture_palette_vram_offset = 0;
    size_t texture_palette_data_size = 0;

    uint32_t filter = pgraph_reg_r(pg, NV_PGRAPH_TEXFILTER0 + texture_idx * 4);
    uint32_t address =
        pgraph_reg_r(pg, NV_PGRAPH_TEXADDRESS0 + texture_idx * 4);
    uint32_t border_color_pack32 =
        pgraph_reg_r(pg, NV_PGRAPH_BORDERCOLOR0 + texture_idx * 4);
    bool is_indexed =
        (state.color_format == NV097_SET_TEXTURE_FORMAT_COLOR_SZ_I8_A8R8G8B8);
    uint32_t max_anisotropy =
        1 << (GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_TEXCTL0_0 + texture_idx * 4),
                       NV_PGRAPH_TEXCTL0_0_MAX_ANISOTROPY));

    TextureKey key;
    memset(&key, 0, sizeof(key));
    key.state = state;
    key.texture_vram_offset = texture_vram_offset;
    key.texture_length = texture_length;
    if (is_indexed) {
        texture_palette_vram_offset =
            pgraph_get_texture_palette_phys_addr_length(
                pg, texture_idx, &texture_palette_data_size);
        key.palette_vram_offset = texture_palette_vram_offset;
        key.palette_length = texture_palette_data_size;
    }
    key.scale = 1;

    // FIXME: Separate sampler from texture
    key.filter = filter;
    key.address = address;
    key.border_color = border_color_pack32;
    key.max_anisotropy = max_anisotropy;

    bool possibly_dirty = false;
    bool possibly_dirty_checked = false;
    bool surface_uploaded = false;
    SurfaceToTextureMode s2t_mode = S2T_NONE;

    // Check active surfaces to see if this texture was a render target
    SurfaceBinding *surface = pgraph_wgpu_surface_get(d, texture_vram_offset);
    if (surface && state.levels == 1) {
        s2t_mode = check_surface_to_texture_compatiblity(surface, &state);

        if (s2t_mode == S2T_NONE && surface->color) {
            trace_nv2a_pgraph_surface_texture_compat_failed(
                surface->shape.color_format, state.color_format);
        }

        if (s2t_mode != S2T_NONE) {
            surface_uploaded = surface->upload_pending;
            pgraph_wgpu_upload_surface_data(d, surface, false);
        }
    }

    bool surface_to_texture = s2t_mode != S2T_NONE;

    if (!surface_to_texture) {
        // FIXME: Restructure to support rendering surfaces to cubemap faces

        // Writeback any surfaces which this texture may index
        if (!surface) {
            pgraph_wgpu_dl_reason = "tex-overlap";
        } else {
            char why[128];
            snprintf(why, sizeof(why),
                     "tex-fallback tex:fmt%u %ux%u p%u lv%u cube%d sw%d",
                     state.color_format, state.width, state.height,
                     state.pitch, state.levels, state.cubemap,
                     surface->swizzle);
            pgraph_wgpu_dl_reason = g_intern_string(why);
        }
        pgraph_wgpu_download_surfaces_in_range_if_dirty(
            pg, texture_vram_offset, texture_length);
    }

    if (surface_to_texture && pg->surface_scale_factor > 1) {
        key.scale = pg->surface_scale_factor;
    }

    uint64_t key_hash = fast_hash((void *)&key, sizeof(key));
    LruNode *node = lru_lookup(&r->tex.texture_cache, key_hash, &key);
    TextureBinding *snode = container_of(node, TextureBinding, node);
    bool binding_found = snode->texture != NULL;

    if (binding_found) {
        r->tex.texture_bindings[texture_idx] = snode;
        possibly_dirty |= snode->possibly_dirty;
    } else {
        possibly_dirty = true;
    }

    if (!surface_to_texture && !possibly_dirty_checked) {
        possibly_dirty |= check_texture_possibly_dirty(
            d, texture_vram_offset, texture_length, texture_palette_vram_offset,
            texture_palette_data_size);
    }

    // Calculate hash of texture data, if necessary
    void *texture_data = (char *)d->vram_ptr + texture_vram_offset;
    void *palette_data = (char *)d->vram_ptr + texture_palette_vram_offset;

    uint64_t content_hash = 0;
    bool refill_from_vram = binding_found && snode->from_surface;
    if (!surface_to_texture && (possibly_dirty || refill_from_vram)) {
        content_hash = fast_hash(texture_data, texture_length);
        if (is_indexed) {
            content_hash ^= fast_hash(palette_data, texture_palette_data_size);
        }
    }

    if (binding_found) {
        if (surface_to_texture) {
            /* An upload can change the contents without a new draw_time. */
            if (surface_uploaded || surface->draw_time != snode->draw_time ||
                !snode->from_surface) {
                copy_surface_to_texture(pg, surface, snode, s2t_mode);
            }
        } else {
            if ((possibly_dirty && content_hash != snode->hash) ||
                refill_from_vram) {
                /* refill_from_vram: last filled from a surface that is no
                 * longer compatible/present */
                upload_texture_image(pg, texture_idx, snode);
                snode->hash = content_hash;
                snode->draw_time = 0;
            }
        }
        snode->possibly_dirty = false;
        return;
    }

    memcpy(&snode->key, &key, sizeof(key));
    snode->possibly_dirty = false;
    snode->hash = content_hash;
    snode->comparison = false;

    const WgpuColorFormatInfo *wf =
        &kelvin_color_format_wgpu_map[state.color_format];
    assert(wf->format != 0);
    assert(1 < state.dimensionality && state.dimensionality <= 3);

    r->tex.texture_bindings[texture_idx] = snode;

    if (surface_to_texture) {
        copy_surface_to_texture(pg, surface, snode, s2t_mode);
    } else {
        upload_texture_image(pg, texture_idx, snode);
        snode->draw_time = 0;
    }

    /* texture format is final now (native BC or not) */
    snode->sampler = create_sampler(r, &key, snode->format, snode->mip_levels);
    snode->sampler_type =
        is_linear_filter_supported_for_format(r, snode->format) ?
            WGPUSamplerBindingType_Filtering :
            WGPUSamplerBindingType_NonFiltering;
}

static bool check_textures_dirty(PGRAPHState *pg)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;

    for (int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        if (!r->tex.texture_bindings[i] || pg->texture_dirty[i]) {
            return true;
        }
    }
    return false;
}

void pgraph_wgpu_bind_textures(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;

    // FIXME: Check for modifications on bind fastpath (CPU hook)
    // FIXME: Mark textures that are sourced from surfaces so we can track them

    r->texture_bindings_changed = false;

    if (!check_textures_dirty(pg)) {
        return;
    }

    for (int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        if (!pgraph_is_texture_enabled(pg, i)) {
            r->tex.texture_bindings[i] = &r->tex.dummy_texture;
            continue;
        }

        create_texture(pg, i);

        pg->texture_dirty[i] = false; // FIXME: Move to renderer?
    }

    r->texture_bindings_changed = true;
}

static void texture_cache_entry_init(Lru *lru, LruNode *node, const void *state)
{
    TextureBinding *snode = container_of(node, TextureBinding, node);

    snode->texture = NULL;
    snode->view = NULL;
    snode->sampler = NULL;
    snode->from_surface = false;
}

static void texture_cache_release_node_resources(TextureBinding *snode)
{
    if (snode->sampler) {
        wgpuSamplerRelease(snode->sampler);
        snode->sampler = NULL;
    }
    release_texture_image(snode);
}

static bool texture_cache_entry_pre_evict(Lru *lru, LruNode *node)
{
    PGRAPHWgpuTextureState *t =
        container_of(lru, PGRAPHWgpuTextureState, texture_cache);
    TextureBinding *snode = container_of(node, TextureBinding, node);

    // Currently bound. Objects used by recorded but unsubmitted commands
    // (vk's submit_time check) stay alive through WebGPU refcounting.
    for (int i = 0; i < ARRAY_SIZE(t->texture_bindings); i++) {
        if (t->texture_bindings[i] == snode) {
            return false;
        }
    }

    return true;
}

static void texture_cache_entry_post_evict(Lru *lru, LruNode *node)
{
    TextureBinding *snode = container_of(node, TextureBinding, node);
    texture_cache_release_node_resources(snode);
}

static bool texture_cache_entry_compare(Lru *lru, LruNode *node,
                                        const void *key)
{
    TextureBinding *snode = container_of(node, TextureBinding, node);
    return memcmp(&snode->key, key, sizeof(TextureKey));
}

static void texture_cache_init(PGRAPHWgpuState *r)
{
    const size_t texture_cache_size = 1024;
    lru_init(&r->tex.texture_cache);
    r->tex.texture_cache_entries =
        g_malloc0_n(texture_cache_size, sizeof(TextureBinding));
    assert(r->tex.texture_cache_entries != NULL);
    for (int i = 0; i < texture_cache_size; i++) {
        lru_add_free(&r->tex.texture_cache,
                     &r->tex.texture_cache_entries[i].node);
    }
    r->tex.texture_cache.init_node = texture_cache_entry_init;
    r->tex.texture_cache.compare_nodes = texture_cache_entry_compare;
    r->tex.texture_cache.pre_node_evict = texture_cache_entry_pre_evict;
    r->tex.texture_cache.post_node_evict = texture_cache_entry_post_evict;
}

static void texture_cache_finalize(PGRAPHWgpuState *r)
{
    lru_flush(&r->tex.texture_cache);
    g_free(r->tex.texture_cache_entries);
    r->tex.texture_cache_entries = NULL;
}

void pgraph_wgpu_trim_texture_cache(PGRAPHState *pg)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;

    // FIXME: Allow specifying some amount to trim by

    int num_to_evict = r->tex.texture_cache.num_used / 4;

    while (num_to_evict-- && lru_try_evict_one(&r->tex.texture_cache)) {
    }
}

void pgraph_wgpu_init_textures(PGRAPHState *pg)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;

    texture_cache_init(r);
    init_surface_to_texture(r);
    create_dummy_texture(r, &r->tex.dummy_texture,
                         WGPUTextureViewDimension_2D);
    create_dummy_texture(r, &r->tex.dummy_texture_cube,
                         WGPUTextureViewDimension_Cube);
    create_dummy_texture(r, &r->tex.dummy_texture_3d,
                         WGPUTextureViewDimension_3D);
}

void pgraph_wgpu_finalize_textures(PGRAPHState *pg)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;

    for (int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        r->tex.texture_bindings[i] = NULL;
    }

    texture_cache_release_node_resources(&r->tex.dummy_texture);
    texture_cache_release_node_resources(&r->tex.dummy_texture_cube);
    texture_cache_release_node_resources(&r->tex.dummy_texture_3d);
    texture_cache_finalize(r);

    assert(r->tex.texture_cache.num_used == 0);

    finalize_surface_to_texture(r);
}
