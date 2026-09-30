/*
 * Geforce NV2A PGRAPH WebGPU Renderer: texture module (owned by the texture module;
 * included from renderer.h, do not include directly)
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#ifndef HW_XBOX_NV2A_PGRAPH_WGPU_TEXTURE_H
#define HW_XBOX_NV2A_PGRAPH_WGPU_TEXTURE_H

/* Same fields and semantics as the Vulkan renderer's TextureKey (hashed) */
typedef struct TextureKey {
    TextureShape state;
    hwaddr texture_vram_offset;
    hwaddr texture_length;
    hwaddr palette_vram_offset;
    hwaddr palette_length;
    float scale;
    uint32_t filter;
    uint32_t address;
    uint32_t border_color;
    uint32_t max_anisotropy;
} TextureKey;

/*
 * A resolved texture stage. The shaders module builds the group-0 bind group
 * (and its layout) from r->tex.texture_bindings[i] using:
 *   view, view_dimension, sample_type   -> texture binding
 *   sampler, sampler_type               -> sampler binding
 *
 * sample_type is Float for 8-bit/BC formats. RGBA32Float textures (Y16
 * depth formats and LU_IMAGE_Y16) are Float only if the device has
 * Float32Filterable, else UnfilterableFloat with a NonFiltering sampler
 * (nearest filtering). X8_Y24 depth textures are R32Uint holding the raw
 * Z24S8 word (like vk's VK_FORMAT_R32_UINT; psh.c samples them through a
 * usampler2D and shifts >> 8) with sample_type Uint and a NonFiltering
 * sampler. No comparison samplers are used: shadow compare is done in the
 * generated pixel shader, as in vk.
 *
 * view (and texture) may be replaced by a re-upload; the texture module sets
 * r->texture_bindings_changed whenever that can happen.
 */
struct TextureBinding {
    LruNode node;
    TextureKey key;

    WGPUTexture texture;
    WGPUTextureView view;
    WGPUSampler sampler;
    WGPUTextureFormat format;
    WGPUTextureViewDimension view_dimension;
    WGPUTextureSampleType sample_type;
    WGPUSamplerBindingType sampler_type;
    bool comparison; /* always false (see above) */

    /* contents were produced from a render surface on the GPU (texture has
     * RenderAttachment/StorageBinding usage and the scaled surface size) */
    bool from_surface;
    unsigned int width, height, depth;
    unsigned int mip_levels, array_layers;

    bool possibly_dirty;
    uint64_t hash;
    int draw_time; /* surface->draw_time of the last surface blit */
};

typedef struct PGRAPHWgpuTextureState {
    Lru texture_cache;
    TextureBinding *texture_cache_entries;
    TextureBinding *texture_bindings[NV2A_MAX_TEXTURES];

    /* opaque white stand-ins for disabled stages (2D, cube, 3D) */
    TextureBinding dummy_texture;
    TextureBinding dummy_texture_cube;
    TextureBinding dummy_texture_3d;

    /* surface -> texture (render-to-texture), see texture.c */
    WGPUShaderModule s2t_color_module, s2t_depth_module, s2t_z24s8_module;
    WGPUBindGroupLayout s2t_color_bgl;   /* texture_2d<f32> */
    WGPUPipelineLayout s2t_color_layout;
    WGPURenderPipeline s2t_color_pipelines[2][2]; /* [BGRA8/RGBA8][opaque] */
    WGPUBindGroupLayout s2t_depth_bgl;   /* texture_depth_2d */
    WGPUPipelineLayout s2t_depth_layout;
    WGPURenderPipeline s2t_depth_pipelines[2]; /* Y16 [fixed/float] */
    WGPUBindGroupLayout s2t_z24s8_bgl;   /* packed words -> r32uint */
    WGPUPipelineLayout s2t_z24s8_layout;
    WGPUComputePipeline s2t_z24s8_pipeline;
} PGRAPHWgpuTextureState;

#endif
