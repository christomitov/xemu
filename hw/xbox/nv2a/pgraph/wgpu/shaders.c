/*
 * Geforce NV2A PGRAPH WebGPU Renderer: shaders module
 *
 * Port of ../vk/shaders.c. Differences from Vulkan:
 *  - no geometry shaders: the generator's .wgsl option replaces the
 *    per-primitive data the GS produced (see glsl/psh.h), primitives are
 *    expanded to lists by the draw module with the provoking vertex first
 *  - no push constants: all uniforms are in two std140 uniform buffers,
 *    whose layout is computed here (identical to the generator's blocks)
 *  - no descriptor sets: a bind group is created whenever the uniform
 *    offsets, textures or layout change
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qemu/fast-hash.h"
#include "qemu/mstring.h"
#include "renderer.h"
#include "qemu/xemu-wasm-stats.h"

#define SHADER_CACHE_SIZE 1024
#define SHADER_MODULE_CACHE_SIZE (4 * 1024)
#define UNIFORM_RING_SIZE (4 * 1024 * 1024)

enum {
    DUMMY_2D,
    DUMMY_CUBE,
    DUMMY_3D,
    DUMMY_2D_UINT,
};

/* ---- std140 uniform blocks ---- */

static void uniform_block_init(ShaderUniformBlock *block,
                               const UniformInfo *info, int count,
                               int skip_index)
{
    size_t offset = 0;

    memset(block, 0, sizeof(*block));
    block->info = info;
    block->count = count;

    for (int i = 0; i < count; i++) {
        size_t size, align;

        if (i == skip_index) {
            block->offset[i] = -1;
            continue;
        }

        switch (info[i].type) {
        case UniformElementType_float:
        case UniformElementType_int:
        case UniformElementType_uint:
            size = align = 4;
            break;
        case UniformElementType_vec2:
        case UniformElementType_ivec2:
            size = align = 8;
            break;
        case UniformElementType_vec3:
            size = 12;
            align = 16;
            break;
        case UniformElementType_vec4:
        case UniformElementType_ivec4:
            size = align = 16;
            break;
        case UniformElementType_mat2:
            /* two vec2 columns, each padded to 16 bytes */
            size = 32;
            align = 16;
            break;
        default:
            g_assert_not_reached();
        }

        if (info[i].count > 1) {
            /* array elements are padded to vec4 */
            align = 16;
            block->stride[i] = ROUND_UP(size, 16);
            size = block->stride[i] * info[i].count;
        } else {
            block->stride[i] = size;
        }

        offset = ROUND_UP(offset, align);
        block->offset[i] = offset;
        offset += size;
    }

    block->total_size = ROUND_UP(MAX(offset, 16), 16);
    block->allocation = g_malloc0(block->total_size);
}

static void uniform_block_finalize(ShaderUniformBlock *block)
{
    g_free(block->allocation);
    block->allocation = NULL;
}

static void uniform_block_store(ShaderUniformBlock *block, const void *values)
{
    for (int i = 0; i < block->count; i++) {
        const UniformInfo *info = &block->info[i];
        if (block->offset[i] < 0) {
            continue;
        }
        const uint8_t *src = (const uint8_t *)values + info->val_offs;
        uint8_t *dst = block->allocation + block->offset[i];
        for (size_t e = 0; e < info->count; e++) {
            if (info->type == UniformElementType_mat2) {
                memcpy(dst, src, 8);
                memcpy(dst + 16, src + 8, 8);
            } else {
                memcpy(dst, src, info->size);
            }
            src += info->size;
            dst += block->stride[i];
        }
    }
}

/* ---- shader modules ---- */

static ShaderModuleInfo *create_shader_module(PGRAPHWgpuState *r,
                                              const ShaderModuleCacheKey *key)
{
    ShaderModuleInfo *info = g_malloc0(sizeof(*info));
    info->stage = key->stage;

    int64_t t0 = g_get_monotonic_time();
    MString *code;
    const char *label;

    if (key->stage == WGPU_SHADER_STAGE_VERTEX) {
        code = pgraph_glsl_gen_vsh(&key->vsh.state, key->vsh.glsl_opts);
        label = "nv2a vsh";
        uniform_block_init(&info->uniforms, VshUniformInfo, VshUniform__COUNT,
                           key->vsh.state.uniform_attrs ? -1 :
                                                          VshUniform_inlineValue);
    } else {
        assert(key->stage == WGPU_SHADER_STAGE_FRAGMENT);
        code = pgraph_glsl_gen_psh(&key->psh.state, key->psh.glsl_opts);
        label = "nv2a psh";
        uniform_block_init(&info->uniforms, PshUniformInfo, PshUniform__COUNT,
                           -1);
    }

    int64_t t1 = g_get_monotonic_time();
    char *wgsl = pgraph_wgpu_glsl_to_wgsl(key->stage, mstring_get_str(code));
    int64_t t2 = g_get_monotonic_time();

    if (wgsl) {
        pgraph_wgpu_reflect_wgsl(wgsl, &info->resources);
        info->module = pgraph_wgpu_create_wgsl_module(r, label, wgsl);
        free(wgsl);
    }
    int64_t t3 = g_get_monotonic_time();
    mstring_unref(code);

    r->shaders.num_compiled++;
    r->shaders.compile_time_us += t3 - t0;
    XSTAT_INC(n_shader_gen);
    XSTAT_ADD(ns_shader_gen, (t3 - t0) * 1000);
    fprintf(stderr,
            "[wgpu] %s #%u: %.1f ms (gen %.1f, glsl->wgsl %.1f, "
            "create %.1f); total %.0f ms%s\n",
            label, r->shaders.num_compiled, (t3 - t0) / 1000.0,
            (t1 - t0) / 1000.0, (t2 - t1) / 1000.0, (t3 - t2) / 1000.0,
            r->shaders.compile_time_us / 1000.0, wgsl ? "" : " FAILED");

    return info;
}

static void ref_shader_module(ShaderModuleInfo *info)
{
    info->refcnt++;
}

static void unref_shader_module(ShaderModuleInfo *info)
{
    assert(info->refcnt >= 1);
    if (--info->refcnt == 0) {
        if (info->module) {
            wgpuShaderModuleRelease(info->module);
        }
        uniform_block_finalize(&info->uniforms);
        g_free(info);
    }
}

static void module_cache_entry_init(Lru *lru, LruNode *node, const void *key)
{
    PGRAPHWgpuShaderState *s =
        container_of(lru, PGRAPHWgpuShaderState, module_cache);
    PGRAPHWgpuState *r = container_of(s, PGRAPHWgpuState, shaders);
    ShaderModuleCacheEntry *entry =
        container_of(node, ShaderModuleCacheEntry, node);

    memcpy(&entry->key, key, sizeof(ShaderModuleCacheKey));
    entry->module_info = create_shader_module(r, &entry->key);
    ref_shader_module(entry->module_info);
}

static void module_cache_entry_post_evict(Lru *lru, LruNode *node)
{
    ShaderModuleCacheEntry *entry =
        container_of(node, ShaderModuleCacheEntry, node);
    unref_shader_module(entry->module_info);
    entry->module_info = NULL;
}

static bool module_cache_entry_compare(Lru *lru, LruNode *node,
                                       const void *key)
{
    ShaderModuleCacheEntry *entry =
        container_of(node, ShaderModuleCacheEntry, node);
    return memcmp(&entry->key, key, sizeof(ShaderModuleCacheKey));
}

static ShaderModuleInfo *get_and_ref_shader_module(PGRAPHWgpuState *r,
                                                   const ShaderModuleCacheKey *key)
{
    uint64_t hash = fast_hash((const uint8_t *)key, sizeof(*key));
    LruNode *node = lru_lookup(&r->shaders.module_cache, hash, key);
    ShaderModuleCacheEntry *entry =
        container_of(node, ShaderModuleCacheEntry, node);
    ref_shader_module(entry->module_info);
    return entry->module_info;
}

/* ---- shader bindings ---- */

static void update_shader_uniform_locs(ShaderBinding *binding)
{
    ShaderUniformBlock *vb = &binding->vsh.module_info->uniforms;
    ShaderUniformBlock *pb = &binding->psh.module_info->uniforms;

    for (int i = 0; i < VshUniform__COUNT; i++) {
        binding->vsh.uniform_locs[i] = vb->offset[i] < 0 ? -1 : i + 1;
    }
    for (int i = 0; i < PshUniform__COUNT; i++) {
        binding->psh.uniform_locs[i] = pb->offset[i] < 0 ? -1 : i + 1;
    }
}

static void shader_cache_entry_init(Lru *lru, LruNode *node, const void *state)
{
    PGRAPHWgpuShaderState *s =
        container_of(lru, PGRAPHWgpuShaderState, shader_cache);
    PGRAPHWgpuState *r = container_of(s, PGRAPHWgpuState, shaders);
    ShaderBinding *binding = container_of(node, ShaderBinding, node);

    memcpy(&binding->state, state, sizeof(ShaderState));
    nv2a_profile_inc_counter(NV2A_PROF_SHADER_GEN);

    ShaderModuleCacheKey key;

    memset(&key, 0, sizeof(key));
    key.stage = WGPU_SHADER_STAGE_VERTEX;
    key.vsh.state = binding->state.vsh;
    key.vsh.glsl_opts.vulkan = true;
    key.vsh.glsl_opts.wgsl = true;
    key.vsh.glsl_opts.prefix_outputs = false;
    key.vsh.glsl_opts.use_push_constants_for_uniform_attrs = false;
    key.vsh.glsl_opts.ubo_binding = WGPU_SHADER_VSH_UBO_BINDING;
    binding->vsh.module_info = get_and_ref_shader_module(r, &key);

    memset(&key, 0, sizeof(key));
    key.stage = WGPU_SHADER_STAGE_FRAGMENT;
    key.psh.state = binding->state.psh;
    key.psh.glsl_opts.vulkan = true;
    key.psh.glsl_opts.wgsl = true;
    key.psh.glsl_opts.ubo_binding = WGPU_SHADER_PSH_UBO_BINDING;
    key.psh.glsl_opts.tex_binding = WGPU_SHADER_TEX_BINDING;
    binding->psh.module_info = get_and_ref_shader_module(r, &key);

    binding->vsh_module = binding->vsh.module_info->module;
    binding->psh_module = binding->psh.module_info->module;
    binding->layout = NULL;
    binding->pipeline_layout = NULL;
    binding->bind_group_layout = NULL;

    update_shader_uniform_locs(binding);
}

static void shader_cache_entry_post_evict(Lru *lru, LruNode *node)
{
    PGRAPHWgpuShaderState *s =
        container_of(lru, PGRAPHWgpuShaderState, shader_cache);
    ShaderBinding *binding = container_of(node, ShaderBinding, node);

    if (s->shader_binding == binding) {
        s->shader_binding = NULL;
    }
    if (binding->vsh.module_info) {
        unref_shader_module(binding->vsh.module_info);
        binding->vsh.module_info = NULL;
    }
    if (binding->psh.module_info) {
        unref_shader_module(binding->psh.module_info);
        binding->psh.module_info = NULL;
    }
    binding->vsh_module = binding->psh_module = NULL;
    binding->layout = NULL;
    binding->pipeline_layout = NULL;
    binding->bind_group_layout = NULL;
}

static bool shader_cache_entry_compare(Lru *lru, LruNode *node, const void *key)
{
    ShaderBinding *binding = container_of(node, ShaderBinding, node);
    return memcmp(&binding->state, key, sizeof(ShaderState));
}

static void shader_cache_init(PGRAPHWgpuShaderState *s)
{
    lru_init(&s->shader_cache);
    s->shader_cache_entries = g_malloc0_n(SHADER_CACHE_SIZE,
                                          sizeof(ShaderBinding));
    for (int i = 0; i < SHADER_CACHE_SIZE; i++) {
        lru_add_free(&s->shader_cache, &s->shader_cache_entries[i].node);
    }
    s->shader_cache.init_node = shader_cache_entry_init;
    s->shader_cache.compare_nodes = shader_cache_entry_compare;
    s->shader_cache.post_node_evict = shader_cache_entry_post_evict;

    lru_init(&s->module_cache);
    s->module_cache_entries = g_malloc0_n(SHADER_MODULE_CACHE_SIZE,
                                          sizeof(ShaderModuleCacheEntry));
    for (int i = 0; i < SHADER_MODULE_CACHE_SIZE; i++) {
        lru_add_free(&s->module_cache, &s->module_cache_entries[i].node);
    }
    s->module_cache.init_node = module_cache_entry_init;
    s->module_cache.compare_nodes = module_cache_entry_compare;
    s->module_cache.post_node_evict = module_cache_entry_post_evict;
}

static void shader_cache_finalize(PGRAPHWgpuShaderState *s)
{
    lru_flush(&s->shader_cache);
    g_free(s->shader_cache_entries);
    s->shader_cache_entries = NULL;

    lru_flush(&s->module_cache);
    g_free(s->module_cache_entries);
    s->module_cache_entries = NULL;
    s->shader_binding = NULL;
}

/* ---- group 0 layouts ---- */

static guint layout_key_hash(gconstpointer key)
{
    return fast_hash((const uint8_t *)key, sizeof(ShaderLayoutKey));
}

static gboolean layout_key_equal(gconstpointer a, gconstpointer b)
{
    return !memcmp(a, b, sizeof(ShaderLayoutKey));
}

static void layout_free(gpointer data)
{
    ShaderLayout *layout = data;
    wgpuPipelineLayoutRelease(layout->pipeline_layout);
    wgpuBindGroupLayoutRelease(layout->bind_group_layout);
    g_free(layout);
}

static WGPUSamplerBindingType sampler_type_for(WGPUTextureSampleType type)
{
    return type == WGPUTextureSampleType_Float ?
               WGPUSamplerBindingType_Filtering :
               WGPUSamplerBindingType_NonFiltering;
}

static ShaderLayout *get_layout(PGRAPHWgpuState *r, const ShaderLayoutKey *key)
{
    ShaderLayout *layout = g_hash_table_lookup(r->shaders.layouts, key);
    if (layout) {
        return layout;
    }

    WGPUBindGroupLayoutEntry entries[2 + 2 * NV2A_MAX_TEXTURES];
    int n = 0;

    entries[n++] = (WGPUBindGroupLayoutEntry){
        .binding = WGPU_SHADER_VSH_UBO_BINDING,
        .visibility = WGPUShaderStage_Vertex,
        .buffer = { .type = WGPUBufferBindingType_Uniform,
                    .hasDynamicOffset = true },
    };
    entries[n++] = (WGPUBindGroupLayoutEntry){
        .binding = WGPU_SHADER_PSH_UBO_BINDING,
        .visibility = WGPUShaderStage_Fragment,
        .buffer = { .type = WGPUBufferBindingType_Uniform,
                    .hasDynamicOffset = true },
    };
    for (int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        if (key->tex_dim[i] == WGPUTextureViewDimension_Undefined) {
            continue;
        }
        entries[n++] = (WGPUBindGroupLayoutEntry){
            .binding = WGPU_SHADER_TEX_BINDING + i,
            .visibility = WGPUShaderStage_Fragment,
            .texture = { .sampleType = key->tex_type[i],
                         .viewDimension = key->tex_dim[i] },
        };
        entries[n++] = (WGPUBindGroupLayoutEntry){
            .binding = WGPU_SHADER_TEX_BINDING + i +
                       WGPU_SHADER_SAMPLER_BINDING_OFFSET,
            .visibility = WGPUShaderStage_Fragment,
            .sampler = { .type = sampler_type_for(key->tex_type[i]) },
        };
    }

    layout = g_new0(ShaderLayout, 1);
    layout->key = *key;
    layout->bind_group_layout = wgpuDeviceCreateBindGroupLayout(
        r->device, &(WGPUBindGroupLayoutDescriptor){
                       .label = { "nv2a group 0", WGPU_STRLEN },
                       .entryCount = n,
                       .entries = entries });
    layout->pipeline_layout = wgpuDeviceCreatePipelineLayout(
        r->device, &(WGPUPipelineLayoutDescriptor){
                       .label = { "nv2a", WGPU_STRLEN },
                       .bindGroupLayoutCount = 1,
                       .bindGroupLayouts = &layout->bind_group_layout });
    g_hash_table_insert(r->shaders.layouts, &layout->key, layout);
    return layout;
}

/* Layout entry sample type for a stage the pixel shader declares. */
static WGPUTextureSampleType layout_sample_type(PGRAPHWgpuState *r, int stage,
                                                WGPUTextureSampleType decl)
{
    if (decl == WGPUTextureSampleType_Float) {
        TextureBinding *tb = r->tex.texture_bindings[stage];
        /* f32 textures that cannot be filtered (e.g. RGBA32Float depth
         * without Float32Filterable) need an unfilterable layout entry */
        if (tb && tb->view &&
            (tb->sample_type == WGPUTextureSampleType_UnfilterableFloat ||
             tb->sample_type == WGPUTextureSampleType_Depth)) {
            return WGPUTextureSampleType_UnfilterableFloat;
        }
    }
    return decl;
}

static void update_layout(PGRAPHWgpuState *r, ShaderBinding *binding)
{
    const ShaderResourceInfo *res = &binding->psh.module_info->resources;
    ShaderLayoutKey key;

    memset(&key, 0, sizeof(key));
    for (int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        if (res->tex_dim[i] == WGPUTextureViewDimension_Undefined) {
            continue;
        }
        key.tex_dim[i] = res->tex_dim[i];
        key.tex_type[i] = layout_sample_type(r, i, res->tex_type[i]);
    }

    if (binding->layout && !memcmp(&binding->layout->key, &key, sizeof(key))) {
        return;
    }

    binding->layout = get_layout(r, &key);
    binding->pipeline_layout = binding->layout->pipeline_layout;
    binding->bind_group_layout = binding->layout->bind_group_layout;
    r->shader_bindings_changed = true;
}

/* ---- uniforms ---- */

static void update_shader_uniforms(PGRAPHState *pg)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;
    ShaderBinding *binding = r->shaders.shader_binding;

    nv2a_profile_inc_counter(NV2A_PROF_SHADER_BIND);

    VshUniformValues vsh_values;
    memset(&vsh_values, 0, sizeof(vsh_values)); /* stable hash */
    pgraph_glsl_set_vsh_uniform_values(pg, &binding->state.vsh,
                                       binding->vsh.uniform_locs, &vsh_values);
    uniform_block_store(&binding->vsh.module_info->uniforms, &vsh_values);

    PshUniformValues psh_values;
    memset(&psh_values, 0, sizeof(psh_values));
    pgraph_glsl_set_psh_uniform_values(pg, binding->psh.uniform_locs,
                                       &psh_values);
    for (int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        TextureBinding *tb = r->tex.texture_bindings[i];
        float scale = 1.0f;
        if (tb) {
            BasicColorFormatInfo f_basic =
                kelvin_color_format_info_map[tb->key.state.color_format];
            if (f_basic.linear) {
                scale = tb->key.scale;
            }
        }
        psh_values.texScale[i] = scale;
    }
    uniform_block_store(&binding->psh.module_info->uniforms, &psh_values);
}

void pgraph_wgpu_flush_uniforms(PGRAPHWgpuState *r)
{
    PGRAPHWgpuShaderState *s = &r->shaders;

    if (s->uniform_staging && s->ustage_hi > s->ustage_lo) {
        wgpuQueueWriteBuffer(r->queue, s->uniform_buffer, s->ustage_lo,
                             s->uniform_staging + s->ustage_lo,
                             ROUND_UP(s->ustage_hi - s->ustage_lo, 4));
    }
    s->ustage_lo = s->ustage_hi = 0;
}

static void create_uniform_buffer(PGRAPHWgpuState *r)
{
    PGRAPHWgpuShaderState *s = &r->shaders;

    if (s->uniform_buffer) {
        /* staged uniforms belong to the old buffer */
        pgraph_wgpu_flush_uniforms(r);
    }
    if (s->uniform_buffer) {
        /* bind groups recorded in the open encoder keep it alive */
        wgpuBufferRelease(s->uniform_buffer);
    }
    s->uniform_buffer = wgpuDeviceCreateBuffer(
        r->device, &(WGPUBufferDescriptor){
                       .label = { "nv2a uniforms", WGPU_STRLEN },
                       .usage = WGPUBufferUsage_Uniform |
                                WGPUBufferUsage_CopyDst,
                       .size = s->uniform_buffer_size });
    s->uniform_offset = 0;
    s->uniforms_uploaded = false;
}

void pgraph_wgpu_shaders_on_submit(PGRAPHState *pg)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;

    /* queue writes issued from now on are ordered after the submitted work */
    r->shaders.uniform_offset = 0;
    r->shaders.uniforms_uploaded = false;
}

static void upload_uniforms(PGRAPHWgpuState *r, ShaderBinding *binding)
{
    PGRAPHWgpuShaderState *s = &r->shaders;
    ShaderUniformBlock *blocks[2] = { &binding->vsh.module_info->uniforms,
                                      &binding->psh.module_info->uniforms };
    uint64_t hashes[2];
    bool changed = !s->uniforms_uploaded;

    for (int i = 0; i < 2; i++) {
        hashes[i] = fast_hash(blocks[i]->allocation, blocks[i]->total_size);
        changed |= hashes[i] != s->uniform_hashes[i];
    }
    if (!changed) {
        nv2a_profile_inc_counter(NV2A_PROF_SHADER_UBO_NOTDIRTY);
        return;
    }
    nv2a_profile_inc_counter(NV2A_PROF_SHADER_UBO_DIRTY);

    size_t needed = 0;
    for (int i = 0; i < 2; i++) {
        needed += ROUND_UP(blocks[i]->total_size, s->uniform_alignment);
    }
    bool all = !s->uniforms_uploaded;
    if (s->uniform_offset + needed > s->uniform_buffer_size) {
        /* Never overwrite regions possibly used by unsubmitted draws */
        create_uniform_buffer(r);
        all = true;     /* the other block's offset is in the old buffer */
    }

    for (int i = 0; i < 2; i++) {
        /* an unchanged block keeps its (still intact) earlier slice */
        if (!all && hashes[i] == s->uniform_hashes[i]) {
            continue;
        }
        s->uniform_block_offsets[i] = s->uniform_offset;
        if (!s->uniform_staging) {
            s->uniform_staging = g_malloc0(s->uniform_buffer_size + 4);
        }
        memcpy(s->uniform_staging + s->uniform_offset, blocks[i]->allocation,
               blocks[i]->total_size);
        if (s->ustage_hi == s->ustage_lo) {
            s->ustage_lo = s->uniform_offset;
        }
        s->ustage_lo = MIN(s->ustage_lo, s->uniform_offset);
        s->ustage_hi = MAX(s->ustage_hi,
                           s->uniform_offset + blocks[i]->total_size);
        s->uniform_offset += ROUND_UP(blocks[i]->total_size,
                                      s->uniform_alignment);
        s->uniform_hashes[i] = hashes[i];
    }
    s->uniforms_uploaded = true;
}

/* ---- bind group ---- */

static int dummy_index(WGPUTextureViewDimension dim, WGPUTextureSampleType type)
{
    if (type == WGPUTextureSampleType_Uint) {
        return DUMMY_2D_UINT;
    }
    switch (dim) {
    case WGPUTextureViewDimension_Cube:
        return DUMMY_CUBE;
    case WGPUTextureViewDimension_3D:
        return DUMMY_3D;
    default:
        return DUMMY_2D;
    }
}

static bool texture_compatible(const TextureBinding *tb,
                               WGPUTextureViewDimension dim,
                               WGPUTextureSampleType type)
{
    if (!tb || !tb->view || !tb->sampler || tb->view_dimension != dim) {
        return false;
    }
    switch (type) {
    case WGPUTextureSampleType_Float:
        return tb->sample_type == WGPUTextureSampleType_Float;
    case WGPUTextureSampleType_UnfilterableFloat:
        return tb->sample_type == WGPUTextureSampleType_Float ||
               tb->sample_type == WGPUTextureSampleType_UnfilterableFloat ||
               tb->sample_type == WGPUTextureSampleType_Depth;
    default:
        return tb->sample_type == type;
    }
}

static void get_texture_for_stage(PGRAPHWgpuState *r, const ShaderLayout *l,
                                  int i, WGPUTextureView *view,
                                  WGPUSampler *sampler)
{
    PGRAPHWgpuShaderState *s = &r->shaders;
    WGPUTextureViewDimension dim = l->key.tex_dim[i];
    WGPUTextureSampleType type = l->key.tex_type[i];
    const TextureBinding *tb = r->tex.texture_bindings[i];

    if (!texture_compatible(tb, dim, type)) {
        /* prefer the texture module's (white) stand-ins */
        const TextureBinding *dummies[] = { &r->tex.dummy_texture,
                                            &r->tex.dummy_texture_cube,
                                            &r->tex.dummy_texture_3d };
        int d = dummy_index(dim, type);
        tb = d < ARRAY_SIZE(dummies) ? dummies[d] : NULL;
        if (!texture_compatible(tb, dim, type)) {
            *view = s->dummy_view[d];
            *sampler = s->dummy_sampler;
            return;
        }
    }

    *view = tb->view;
    *sampler = tb->sampler;
    if (sampler_type_for(type) == WGPUSamplerBindingType_NonFiltering &&
        tb->sampler_type != WGPUSamplerBindingType_NonFiltering) {
        *sampler = s->dummy_sampler;
    }
}

static void bg_cache_entry_release(struct WgpuBindGroupEntry *e)
{
    if (!e->bind_group) {
        return;
    }
    wgpuBindGroupRelease(e->bind_group);
    wgpuBindGroupLayoutRelease(e->layout);
    wgpuBufferRelease(e->buffer);
    for (int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        if (e->views[i]) {
            wgpuTextureViewRelease(e->views[i]);
        }
        if (e->samplers[i]) {
            wgpuSamplerRelease(e->samplers[i]);
        }
    }
    memset(e, 0, sizeof(*e));
}

WGPUBindGroup pgraph_wgpu_update_bind_group(PGRAPHState *pg)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;
    PGRAPHWgpuShaderState *s = &r->shaders;
    ShaderBinding *binding = s->shader_binding;

    assert(binding);
    if (!binding->layout) {
        update_layout(r, binding);
    }
    upload_uniforms(r, binding);

    const ShaderLayout *l = binding->layout;
    ShaderUniformBlock *blocks[2] = { &binding->vsh.module_info->uniforms,
                                      &binding->psh.module_info->uniforms };
    size_t sizes[2] = { blocks[0]->total_size, blocks[1]->total_size };
    WGPUTextureView views[NV2A_MAX_TEXTURES] = { 0 };
    WGPUSampler samplers[NV2A_MAX_TEXTURES] = { 0 };
    for (int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        if (l->key.tex_dim[i] != WGPUTextureViewDimension_Undefined) {
            get_texture_for_stage(r, l, i, &views[i], &samplers[i]);
        }
    }

    /*
     * The uniform blocks are bound with dynamic offsets (passed to
     * SetBindGroup per draw, see pgraph_wgpu_uniform_offsets), so changing
     * uniforms no longer needs a new bind group: previously matrices
     * changing per draw meant CreateBindGroup + Release on every draw.
     */
    if (s->bind_group && s->bind_group_layout == l &&
        s->bind_group_buffer == s->uniform_buffer &&
        !memcmp(s->bind_group_sizes, sizes, sizeof(sizes)) &&
        !memcmp(s->bind_group_views, views, sizeof(views)) &&
        !memcmp(s->bind_group_samplers, samplers, sizeof(samplers))) {
        return s->bind_group;
    }

    struct WgpuBindGroupEntry *e = NULL;
    for (int k = 0; k < WGPU_BIND_GROUP_CACHE; k++) {
        struct WgpuBindGroupEntry *c = &s->bg_cache[k];
        if (c->bind_group && c->layout == l->bind_group_layout &&
            c->buffer == s->uniform_buffer &&
            !memcmp(c->sizes, sizes, sizeof(sizes)) &&
            !memcmp(c->views, views, sizeof(views)) &&
            !memcmp(c->samplers, samplers, sizeof(samplers))) {
            e = c;
            break;
        }
    }

    if (!e) {
        WGPUBindGroupEntry entries[2 + 2 * NV2A_MAX_TEXTURES];
        int n = 0;

        for (int i = 0; i < 2; i++) {
            entries[n++] = (WGPUBindGroupEntry){
                .binding = i == 0 ? WGPU_SHADER_VSH_UBO_BINDING :
                                    WGPU_SHADER_PSH_UBO_BINDING,
                .buffer = s->uniform_buffer,
                .offset = 0,
                .size = sizes[i],
            };
        }
        for (int i = 0; i < NV2A_MAX_TEXTURES; i++) {
            if (!views[i]) {
                continue;
            }
            entries[n++] = (WGPUBindGroupEntry){
                .binding = WGPU_SHADER_TEX_BINDING + i,
                .textureView = views[i],
            };
            entries[n++] = (WGPUBindGroupEntry){
                .binding = WGPU_SHADER_TEX_BINDING + i +
                           WGPU_SHADER_SAMPLER_BINDING_OFFSET,
                .sampler = samplers[i],
            };
        }

        XSTAT_INC(n_bind_group_create);
        WGPUBindGroup bg = wgpuDeviceCreateBindGroup(
            r->device, &(WGPUBindGroupDescriptor){
                           .layout = l->bind_group_layout,
                           .entryCount = n,
                           .entries = entries });
        if (!bg) {
            return NULL;
        }
        e = &s->bg_cache[s->bg_cache_next++ % WGPU_BIND_GROUP_CACHE];
        /* command encoders keep a reference while it is in use */
        bg_cache_entry_release(e);
        e->bind_group = bg;
        e->layout = l->bind_group_layout;
        wgpuBindGroupLayoutAddRef(e->layout);
        e->buffer = s->uniform_buffer;
        wgpuBufferAddRef(e->buffer);
        memcpy(e->sizes, sizes, sizeof(sizes));
        memcpy(e->views, views, sizeof(views));
        memcpy(e->samplers, samplers, sizeof(samplers));
        for (int i = 0; i < NV2A_MAX_TEXTURES; i++) {
            if (views[i]) {
                wgpuTextureViewAddRef(views[i]);
            }
            if (samplers[i]) {
                wgpuSamplerAddRef(samplers[i]);
            }
        }
    }

    s->bind_group_gen++;
    s->bind_group = e->bind_group;
    s->bind_group_layout = binding->layout;
    s->bind_group_buffer = s->uniform_buffer;
    memcpy(s->bind_group_offsets, s->uniform_block_offsets,
           sizeof(s->bind_group_offsets));
    memcpy(s->bind_group_sizes, sizes, sizeof(sizes));
    memcpy(s->bind_group_views, views, sizeof(views));
    memcpy(s->bind_group_samplers, samplers, sizeof(samplers));

    return s->bind_group;
}

/* Dynamic offsets for bind group 0, in binding order (VSH UBO, PSH UBO). */
void pgraph_wgpu_uniform_offsets(PGRAPHState *pg, uint32_t out[2])
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;

    QEMU_BUILD_BUG_ON(WGPU_SHADER_VSH_UBO_BINDING != 0 ||
                      WGPU_SHADER_PSH_UBO_BINDING != 1);
    out[0] = r->shaders.uniform_block_offsets[0];
    out[1] = r->shaders.uniform_block_offsets[1];
}

/* ---- binding ---- */

static ShaderBinding *get_shader_binding_for_state(PGRAPHWgpuState *r,
                                                   const ShaderState *state)
{
    uint64_t hash = fast_hash((const uint8_t *)state, sizeof(*state));
    LruNode *node = lru_lookup(&r->shaders.shader_cache, hash, state);
    return container_of(node, ShaderBinding, node);
}

void pgraph_wgpu_bind_shaders(PGRAPHState *pg)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;
    PGRAPHWgpuShaderState *s = &r->shaders;

    r->shader_bindings_changed = false;

    if (!s->shader_binding ||
        pgraph_glsl_check_shader_state_dirty(pg, &s->shader_binding->state)) {
        ShaderState new_state = pgraph_glsl_get_shader_state(pg);
        if (!s->shader_binding || memcmp(&s->shader_binding->state, &new_state,
                                         sizeof(ShaderState))) {
            s->shader_binding = get_shader_binding_for_state(r, &new_state);
            r->shader_bindings_changed = true;
        }
    } else {
        nv2a_profile_inc_counter(NV2A_PROF_SHADER_BIND_NOTDIRTY);
    }

    /* texture sample types may change the layout (see shaders.h) */
    update_layout(r, s->shader_binding);
    update_shader_uniforms(pg);
}

/* ---- init / finalize ---- */

static void create_dummies(PGRAPHWgpuState *r)
{
    PGRAPHWgpuShaderState *s = &r->shaders;
    static const struct {
        WGPUTextureDimension dim;
        WGPUTextureViewDimension view_dim;
        uint32_t layers;
        WGPUTextureFormat format;
    } desc[] = {
        [DUMMY_2D] = { WGPUTextureDimension_2D, WGPUTextureViewDimension_2D,
                       1, WGPUTextureFormat_RGBA8Unorm },
        [DUMMY_CUBE] = { WGPUTextureDimension_2D,
                         WGPUTextureViewDimension_Cube, 6,
                         WGPUTextureFormat_RGBA8Unorm },
        [DUMMY_3D] = { WGPUTextureDimension_3D, WGPUTextureViewDimension_3D,
                       1, WGPUTextureFormat_RGBA8Unorm },
        [DUMMY_2D_UINT] = { WGPUTextureDimension_2D,
                            WGPUTextureViewDimension_2D, 1,
                            WGPUTextureFormat_R32Uint },
    };

    for (int i = 0; i < ARRAY_SIZE(desc); i++) {
        s->dummy_texture[i] = wgpuDeviceCreateTexture(
            r->device, &(WGPUTextureDescriptor){
                           .label = { "nv2a shader dummy", WGPU_STRLEN },
                           .usage = WGPUTextureUsage_TextureBinding,
                           .dimension = desc[i].dim,
                           .size = { 1, 1, desc[i].layers },
                           .format = desc[i].format,
                           .mipLevelCount = 1,
                           .sampleCount = 1 });
        s->dummy_view[i] = wgpuTextureCreateView(
            s->dummy_texture[i],
            &(WGPUTextureViewDescriptor){
                .format = desc[i].format,
                .dimension = desc[i].view_dim,
                .baseMipLevel = 0,
                .mipLevelCount = 1,
                .baseArrayLayer = 0,
                .arrayLayerCount = desc[i].layers,
                .aspect = WGPUTextureAspect_All,
                .usage = WGPUTextureUsage_TextureBinding });
    }

    s->dummy_sampler = wgpuDeviceCreateSampler(
        r->device, &(WGPUSamplerDescriptor){
                       .label = { "nv2a shader dummy", WGPU_STRLEN },
                       .addressModeU = WGPUAddressMode_ClampToEdge,
                       .addressModeV = WGPUAddressMode_ClampToEdge,
                       .addressModeW = WGPUAddressMode_ClampToEdge,
                       .magFilter = WGPUFilterMode_Nearest,
                       .minFilter = WGPUFilterMode_Nearest,
                       .mipmapFilter = WGPUMipmapFilterMode_Nearest,
                       .lodMinClamp = 0.0f,
                       .lodMaxClamp = 32.0f,
                       .maxAnisotropy = 1 });
}

void pgraph_wgpu_init_shaders(PGRAPHState *pg)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;
    PGRAPHWgpuShaderState *s = &r->shaders;

    pgraph_wgpu_init_glsl_compiler();
    shader_cache_init(s);
    s->layouts = g_hash_table_new_full(layout_key_hash, layout_key_equal,
                                       NULL, layout_free);

    s->uniform_alignment = MAX(r->limits.minUniformBufferOffsetAlignment, 16);
    s->uniform_buffer_size = UNIFORM_RING_SIZE;
    create_uniform_buffer(r);
    create_dummies(r);
}

void pgraph_wgpu_finalize_shaders(PGRAPHState *pg)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;
    PGRAPHWgpuShaderState *s = &r->shaders;

    for (int k = 0; k < WGPU_BIND_GROUP_CACHE; k++) {
        bg_cache_entry_release(&s->bg_cache[k]);
    }
    if (s->bind_group) {
        s->bind_group = NULL;
    }
    shader_cache_finalize(s);
    g_hash_table_destroy(s->layouts);
    s->layouts = NULL;

    for (int i = 0; i < ARRAY_SIZE(s->dummy_texture); i++) {
        wgpuTextureViewRelease(s->dummy_view[i]);
        wgpuTextureRelease(s->dummy_texture[i]);
        s->dummy_view[i] = NULL;
        s->dummy_texture[i] = NULL;
    }
    wgpuSamplerRelease(s->dummy_sampler);
    s->dummy_sampler = NULL;

    if (s->uniform_buffer) {
        wgpuBufferRelease(s->uniform_buffer);
        g_free(s->uniform_staging);
        s->uniform_staging = NULL;
        s->uniform_buffer = NULL;
    }
}
