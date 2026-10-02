/*
 * Geforce NV2A PGRAPH WebGPU Renderer: GLSL -> WGSL toolchain glue
 *
 * GLSL (vulkan semantics) is compiled to SPIR-V by glslang and translated to
 * WGSL by Tint, both built into the wasm binary (wgsl-toolchain,
 * xemu_wgsl.h). Translation is synchronous and runs on the pfifo thread.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#include "renderer.h"
#include "qemu/xemu-wasm-stats.h"
#include "xemu_wgsl.h"

void pgraph_wgpu_init_glsl_compiler(void)
{
    if (xemu_wgsl_init() != 0) {
        fprintf(stderr, "[wgpu] xemu_wgsl_init failed\n");
        return;
    }

    XemuWgslOptions opts;
    xemu_wgsl_get_default_options(&opts);
    /* combined sampler at (0, b) -> texture (0, b) + sampler (0, b + 16) */
    opts.sampler_binding_offset = WGPU_SHADER_SAMPLER_BINDING_OFFSET;
    /* the nv2a shaders never use push constants (UBO path only) */
    opts.push_constant_group = 1;
    opts.push_constant_binding = 0;
    opts.allow_immediate_address_space = 0;
    opts.strip_point_size = 1;
    xemu_wgsl_set_options(&opts);
}

static void log_source(const char *src)
{
    int line = 1;
    const char *p = src;

    while (*p) {
        const char *nl = strchr(p, '\n');
        int len = nl ? (int)(nl - p) : (int)strlen(p);
        fprintf(stderr, "%4d: %.*s\n", line++, len, p);
        if (!nl) {
            break;
        }
        p = nl + 1;
    }
}

/*
 * GLSL -> WGSL results, keyed by stage + SHA-1 of the GLSL, as files in
 * /xemu/wgsl/. The page restores the directory from the Cache API before
 * boot and saves new entries, so shaders seen on an earlier visit skip
 * glslang + Tint. Bump WGSL_CACHE_VERSION when translation output changes.
 * XEMU_WASM_WGSL_CACHE=0 disables.
 */
#define WGSL_CACHE_VERSION "1"

static char *wgsl_cache_path(int stage, const char *glsl)
{
    static int enabled = -1;
    if (enabled < 0) {
        const char *e = getenv("XEMU_WASM_WGSL_CACHE");
        enabled = !(e && *e == '0');
        if (enabled) {
            g_mkdir_with_parents("/xemu/wgsl", 0755);
        }
    }
    if (!enabled) {
        return NULL;
    }
    g_autofree char *sum = g_compute_checksum_for_string(G_CHECKSUM_SHA1,
                                                         glsl, -1);
    return g_strdup_printf("/xemu/wgsl/v" WGSL_CACHE_VERSION "-%d-%s.wgsl",
                           stage, sum);
}

/* The persistent WGSL cache: NULL on a miss. */
char *pgraph_wgpu_wgsl_cache_lookup(int stage, const char *glsl)
{
    g_autofree char *cache = wgsl_cache_path(stage, glsl);
    g_autofree char *hit = NULL;
    if (cache && g_file_get_contents(cache, &hit, NULL, NULL) && hit[0]) {
        XSTAT_INC(n_wgsl_cache_hit);
        return strdup(hit);
    }
    return NULL;
}

void pgraph_wgpu_wgsl_cache_store(int stage, const char *glsl, const char *wgsl)
{
    g_autofree char *cache = wgsl_cache_path(stage, glsl);
    if (cache && wgsl) {
        g_file_set_contents(cache, wgsl, -1, NULL);
    }
}

/*
 * glslang + Tint without the cache. Serialized: the GPU thread and the async
 * translation worker (shaders.c) both call it; glslang keeps process state.
 */
char *pgraph_wgpu_translate_glsl(int stage, const char *glsl, char **err)
{
    static QemuMutex lock;
    static gsize once;
    if (g_once_init_enter(&once)) {
        qemu_mutex_init(&lock);
        g_once_init_leave(&once, 1);
    }
    qemu_mutex_lock(&lock);
    char *wgsl = xemu_glsl_to_wgsl(stage, glsl, err);
    qemu_mutex_unlock(&lock);
    return wgsl;
}

char *pgraph_wgpu_glsl_to_wgsl(int stage, const char *glsl)
{
    char *err = NULL;
    g_autofree char *cache = wgsl_cache_path(stage, glsl);
    if (cache) {
        g_autofree char *hit = NULL;
        if (g_file_get_contents(cache, &hit, NULL, NULL) && hit[0]) {
            XSTAT_INC(n_wgsl_cache_hit);
            return strdup(hit);
        }
    }
    /* Tint aborts the process on an internal compiler error; keep the input
     * on MEMFS so the offending shader can be inspected after a crash
     * (XEMU_WASM_KEEP_SHADER=1; each write is a synchronous MEMFS trip). */
    if (getenv("XEMU_WASM_KEEP_SHADER")) {
        FILE *f = fopen("/xemu/last-shader.glsl", "w");
        if (f) {
            fprintf(f, "// stage %d\n%s", stage, glsl);
            fclose(f);
        }
    }
    char *wgsl = pgraph_wgpu_translate_glsl(stage, glsl, &err);
    if (wgsl && cache) {
        g_file_set_contents(cache, wgsl, -1, NULL);
    }

    if (!wgsl) {
        fprintf(stderr, "[wgpu] GLSL -> WGSL translation failed (stage %d):\n"
                "%s\n", stage, err ? err : "(no message)");
        log_source(glsl);
        free(err);
        return NULL;
    }
    free(err);
    return wgsl;
}

WGPUShaderModule pgraph_wgpu_create_shader_module_from_glsl(
    PGRAPHWgpuState *r, int stage, const char *label, const char *glsl)
{
    char *wgsl = pgraph_wgpu_glsl_to_wgsl(stage, glsl);
    if (!wgsl) {
        return NULL;
    }
    WGPUShaderModule module = pgraph_wgpu_create_wgsl_module(r, label, wgsl);
    free(wgsl);
    return module;
}

static WGPUTextureSampleType parse_sample_type(const char *t, const char *end)
{
    const char *lt = memchr(t, '<', end - t);
    if (!lt) {
        return WGPUTextureSampleType_Float;
    }
    if (!strncmp(lt, "<u32", 4)) {
        return WGPUTextureSampleType_Uint;
    }
    if (!strncmp(lt, "<i32", 4)) {
        return WGPUTextureSampleType_Sint;
    }
    return WGPUTextureSampleType_Float;
}

/*
 * Find the group 0 resources of a Tint-generated WGSL module. Tint emits one
 * declaration per line:
 *   @group(0u) @binding(5u) var texSamp3_image : texture_2d<f32>;
 *   @group(0u) @binding(1u) var<uniform> v : PshUniforms_1;
 */
void pgraph_wgpu_reflect_wgsl(const char *wgsl, ShaderResourceInfo *info)
{
    memset(info, 0, sizeof(*info));

    for (const char *p = wgsl; (p = strstr(p, "@group(")) != NULL;) {
        const char *eol = strchr(p, '\n');
        if (!eol) {
            eol = p + strlen(p);
        }
        unsigned long group = strtoul(p + 7, NULL, 10);
        const char *b = strstr(p, "@binding(");
        const char *var = strstr(p, " var");
        if (group != 0 || !b || b > eol || !var || var > eol) {
            p = eol;
            continue;
        }
        unsigned long binding = strtoul(b + 9, NULL, 10);
        const char *colon = memchr(var, ':', eol - var);
        const char *type = colon ? colon + 1 : eol;
        while (type < eol && *type == ' ') {
            type++;
        }

        if (!strncmp(var, " var<uniform>", 13)) {
            info->has_ubo = true;
            info->ubo_binding = binding;
        } else if (binding >= WGPU_SHADER_TEX_BINDING &&
                   binding < WGPU_SHADER_TEX_BINDING + NV2A_MAX_TEXTURES &&
                   !strncmp(type, "texture_", 8)) {
            int i = binding - WGPU_SHADER_TEX_BINDING;
            const char *t = type + 8;
            WGPUTextureSampleType st = parse_sample_type(t, eol);
            if (!strncmp(t, "depth_", 6)) {
                t += 6;
                st = WGPUTextureSampleType_Depth;
            }
            if (!strncmp(t, "2d_array", 8)) {
                info->tex_dim[i] = WGPUTextureViewDimension_2DArray;
            } else if (!strncmp(t, "2d", 2)) {
                info->tex_dim[i] = WGPUTextureViewDimension_2D;
            } else if (!strncmp(t, "3d", 2)) {
                info->tex_dim[i] = WGPUTextureViewDimension_3D;
            } else if (!strncmp(t, "cube_array", 10)) {
                info->tex_dim[i] = WGPUTextureViewDimension_CubeArray;
            } else if (!strncmp(t, "cube", 4)) {
                info->tex_dim[i] = WGPUTextureViewDimension_Cube;
            } else if (!strncmp(t, "1d", 2)) {
                info->tex_dim[i] = WGPUTextureViewDimension_1D;
            } else {
                fprintf(stderr, "[wgpu] unexpected WGSL texture type: %.*s\n",
                        (int)(eol - type), type);
                p = eol;
                continue;
            }
            info->tex_type[i] = st;
        }
        p = eol;
    }
}
