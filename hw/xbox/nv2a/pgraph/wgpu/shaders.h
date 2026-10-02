/*
 * Geforce NV2A PGRAPH WebGPU Renderer: shaders module (owned by the shaders
 * module; included from renderer.h, do not include directly)
 *
 * Shaders are produced by xemu's GLSL generator (vulkan flavour, with the
 * option-gated .wgsl variant: no geometry shader, no gl_PointCoord, no
 * isnan/isinf) and translated GLSL -> SPIR-V (glslang) -> WGSL (Tint) by the
 * wgsl-toolchain (xemu_wgsl.h). Both entry points are named "main".
 *
 * Bind group 0 (the only group; see pgraph_wgpu_update_bind_group):
 *   binding 0          uniform buffer VshUniforms (vertex)
 *   binding 1          uniform buffer PshUniforms (fragment)
 *   binding 2 + i      texture of stage i (fragment), i < 4
 *   binding 18 + i     sampler of stage i (fragment), i.e. texture + 16
 * Only texture stages declared by the pixel shader are present in the
 * layout; their view dimension comes from the shader, their sample type
 * (Float / UnfilterableFloat / Uint) from the shader and the currently bound
 * TextureBinding, so ShaderBinding.pipeline_layout may change without the
 * ShaderBinding changing (r->shader_bindings_changed is set when it does).
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#ifndef HW_XBOX_NV2A_PGRAPH_WGPU_SHADERS_H
#define HW_XBOX_NV2A_PGRAPH_WGPU_SHADERS_H

#define WGPU_SHADER_VSH_UBO_BINDING 0
#define WGPU_SHADER_PSH_UBO_BINDING 1
#define WGPU_SHADER_TEX_BINDING 2
#define WGPU_SHADER_SAMPLER_BINDING_OFFSET 16

/* xemu_wgsl stage numbers */
enum {
    WGPU_SHADER_STAGE_VERTEX = 0,
    WGPU_SHADER_STAGE_FRAGMENT = 1,
    WGPU_SHADER_STAGE_COMPUTE = 2,
};

#define WGPU_SHADER_MAX_UNIFORMS \
    ((int)VshUniform__COUNT > (int)PshUniform__COUNT ? \
         (int)VshUniform__COUNT : (int)PshUniform__COUNT)

/* std140 layout of a generator uniform block (VshUniforms / PshUniforms),
 * indexed like VshUniformInfo / PshUniformInfo */
typedef struct ShaderUniformBlock {
    const UniformInfo *info;
    int count;
    int32_t offset[WGPU_SHADER_MAX_UNIFORMS]; /* -1: not in block */
    uint32_t stride[WGPU_SHADER_MAX_UNIFORMS]; /* array element stride */
    size_t total_size;
    uint8_t *allocation; /* CPU copy, total_size bytes */
} ShaderUniformBlock;

/* Resource bindings declared by a translated WGSL module (group 0) */
typedef struct ShaderResourceInfo {
    bool has_ubo;
    uint32_t ubo_binding;
    /* per texture stage: Undefined if the stage is not declared */
    WGPUTextureViewDimension tex_dim[NV2A_MAX_TEXTURES];
    /* Float (f32), Uint (u32), Sint (i32) or Depth (texture_depth_*) */
    WGPUTextureSampleType tex_type[NV2A_MAX_TEXTURES];
} ShaderResourceInfo;

typedef struct ShaderModuleInfo {
    int refcnt;
    int stage;
    WGPUShaderModule module; /* NULL if translation failed or still pending */
    /*
     * Async translation (cache miss): the worker fills async_wgsl and sets
     * async_done; the GPU thread then creates the module. Draws needing a
     * pending module are skipped instead of stalling the frame.
     */
    bool pending;
    int async_done;
    char *async_glsl;
    char *async_wgsl;
    const char *label;
    ShaderResourceInfo resources;
    ShaderUniformBlock uniforms;
} ShaderModuleInfo;

typedef struct ShaderModuleCacheKey {
    int stage;
    union {
        struct {
            VshState state;
            GenVshGlslOptions glsl_opts;
        } vsh;
        struct {
            PshState state;
            GenPshGlslOptions glsl_opts;
        } psh;
    };
} ShaderModuleCacheKey;

typedef struct ShaderModuleCacheEntry {
    LruNode node;
    ShaderModuleCacheKey key;
    ShaderModuleInfo *module_info;
} ShaderModuleCacheEntry;

/* Texture part of the group 0 layout (memcmp/hash-able) */
typedef struct ShaderLayoutKey {
    uint8_t tex_dim[NV2A_MAX_TEXTURES];  /* WGPUTextureViewDimension */
    uint8_t tex_type[NV2A_MAX_TEXTURES]; /* WGPUTextureSampleType */
} ShaderLayoutKey;

typedef struct ShaderLayout {
    ShaderLayoutKey key;
    WGPUBindGroupLayout bind_group_layout;
    WGPUPipelineLayout pipeline_layout;
} ShaderLayout;

struct ShaderBinding {
    LruNode node;
    ShaderState state;

    /* used by the draw module; entry point "main" for both. Either module
     * may be NULL if shader translation failed: skip the draw then. */
    WGPUShaderModule vsh_module, psh_module;
    /* group 0 layout for the current texture bindings; include it in the
     * render pipeline cache key */
    WGPUPipelineLayout pipeline_layout;
    WGPUBindGroupLayout bind_group_layout;
    ShaderLayout *layout;

    struct {
        ShaderModuleInfo *module_info;
        VshUniformLocs uniform_locs;
    } vsh;
    struct {
        ShaderModuleInfo *module_info;
        PshUniformLocs uniform_locs;
    } psh;
};

typedef struct PGRAPHWgpuShaderState {
    Lru shader_cache;
    ShaderBinding *shader_cache_entries;
    Lru module_cache;
    ShaderModuleCacheEntry *module_cache_entries;
    ShaderBinding *shader_binding; /* current */

    GHashTable *layouts; /* ShaderLayoutKey -> ShaderLayout */

    /*
     * Uniform ring: every uniform change appends both blocks at
     * minUniformBufferOffsetAlignment. A region is never rewritten before
     * the command buffer using it is submitted (queue writes land before the
     * next submit): the offset only resets in pgraph_wgpu_shaders_on_submit;
     * when the ring is full a fresh buffer replaces it (bind groups keep the
     * old one alive).
     */
    WGPUBuffer uniform_buffer;
    size_t uniform_buffer_size;
    /*
     * Uniform uploads are staged here (same offsets as the buffer) and the
     * dirty range goes to the GPU with one writeBuffer before the submit
     * (or before the buffer is replaced): see pgraph_wgpu_flush_uniforms.
     */
    uint8_t *uniform_staging;
    size_t ustage_lo, ustage_hi;
    size_t uniform_offset;
    uint32_t uniform_alignment;
    size_t uniform_block_offsets[2]; /* vsh, psh of the last upload */
    uint64_t uniform_hashes[2];     /* unused (see uniform_last) */
    /* copies of the last uploaded blocks, compared instead of hashed */
    uint8_t *uniform_last[2];
    size_t uniform_last_size[2];
    bool uniforms_uploaded; /* last upload is valid for the current ring */

    /*
     * Recently created bind groups and their inputs, so that draws
     * alternating between a few texture sets reuse them. Entries hold a
     * reference on every handle in their key, so a released handle cannot
     * be reused for another object while an entry still matches it.
     */
#define WGPU_BIND_GROUP_CACHE 32
    struct WgpuBindGroupEntry {
        WGPUBindGroup bind_group;
        WGPUBindGroupLayout layout;
        WGPUBuffer buffer;
        size_t sizes[2];
        WGPUTextureView views[NV2A_MAX_TEXTURES];
        WGPUSampler samplers[NV2A_MAX_TEXTURES];
    } bg_cache[WGPU_BIND_GROUP_CACHE];
    unsigned int bg_cache_next;

    /* last bind group and its inputs (bind_group is owned by bg_cache) */
    WGPUBindGroup bind_group;
    ShaderLayout *bind_group_layout;
    WGPUBuffer bind_group_buffer;
    size_t bind_group_offsets[2];
    size_t bind_group_sizes[2];
    WGPUTextureView bind_group_views[NV2A_MAX_TEXTURES];
    WGPUSampler bind_group_samplers[NV2A_MAX_TEXTURES];

    /* fallbacks for declared stages without a compatible texture:
     * [0] 2D f32, [1] cube f32, [2] 3D f32, [3] 2D u32 */
    WGPUTexture dummy_texture[4];
    WGPUTextureView dummy_view[4];
    WGPUSampler dummy_sampler; /* nearest, usable with any binding type */

    /* statistics */
    unsigned int num_compiled;
    int64_t compile_time_us;
    /* bumped on every bind group (re)creation: handles can be reused */
    uint32_t bind_group_gen;
} PGRAPHWgpuShaderState;

/*
 * Extra entry points of the shaders module (not in renderer.h yet):
 */

/* Call after every queue submit of the draw encoder (e.g. from
 * pgraph_wgpu_finish): lets the uniform ring be reused. Optional; without it
 * a new ring buffer is allocated whenever the ring fills up. */
void pgraph_wgpu_shaders_on_submit(PGRAPHState *pg);

/* GLSL (vulkan semantics) -> WGSL -> WGPUShaderModule, for other modules'
 * helper shaders. stage: WGPU_SHADER_STAGE_*. Returns NULL on failure (the
 * error and source are logged). Samplers: see xemu_wgsl.h. */
WGPUShaderModule pgraph_wgpu_create_shader_module_from_glsl(
    PGRAPHWgpuState *r, int stage, const char *label, const char *glsl);

/* glsl.c internals */
void pgraph_wgpu_init_glsl_compiler(void);
char *pgraph_wgpu_glsl_to_wgsl(int stage, const char *glsl);
char *pgraph_wgpu_wgsl_cache_lookup(int stage, const char *glsl);
void pgraph_wgpu_wgsl_cache_store(int stage, const char *glsl, const char *wgsl);
char *pgraph_wgpu_translate_glsl(int stage, const char *glsl, char **err);
void pgraph_wgpu_reflect_wgsl(const char *wgsl, ShaderResourceInfo *info);

#endif
