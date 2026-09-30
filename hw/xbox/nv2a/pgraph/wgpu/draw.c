/*
 * Geforce NV2A PGRAPH WebGPU Renderer: draws, clears and pipelines
 *
 * Port of vk/draw.c. Differences from the Vulkan renderer:
 *  - no geometry shaders: every NV2A primitive is converted on the CPU into
 *    an index list of a WebGPU-supported topology (point/line/triangle
 *    list), with the provoking vertex first in each primitive (WGSL flat
 *    interpolation uses the first vertex), preserving winding. See
 *    gen_prim_indices() for the table. All draws are therefore indexed.
 *  - no render pass / framebuffer objects: a WGPURenderPassEncoder is begun
 *    on the color/zeta SurfaceBinding views and restarted when the surface
 *    module marks the framebuffer dirty or the attachments change.
 *  - no vkCmdClearAttachments: full-surface clears of whole attachments /
 *    aspects use loadOp Clear, everything else draws a full-screen triangle
 *    with a clear pipeline (color via blend constant + write mask, depth via
 *    the vertex z, stencil via reference + Replace) and the clear rect as
 *    scissor.
 *  - no push constants: uniform attributes and all other uniforms are
 *    uploaded by the shaders module (pgraph_wgpu_update_bind_group).
 *
 * Copyright (c) 2024-2025 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qemu/fast-hash.h"
#include "renderer.h"
#include "qemu/xemu-wasm-stats.h"
#include <math.h>

/* ---- NV2A -> WebGPU state maps (cf. vk/constants.h) ---- */

static const WGPUBlendFactor pgraph_blend_factor_wgpu_map[] = {
    WGPUBlendFactor_Zero,
    WGPUBlendFactor_One,
    WGPUBlendFactor_Src,
    WGPUBlendFactor_OneMinusSrc,
    WGPUBlendFactor_SrcAlpha,
    WGPUBlendFactor_OneMinusSrcAlpha,
    WGPUBlendFactor_DstAlpha,
    WGPUBlendFactor_OneMinusDstAlpha,
    WGPUBlendFactor_Dst,
    WGPUBlendFactor_OneMinusDst,
    WGPUBlendFactor_SrcAlphaSaturated,
    WGPUBlendFactor_Zero, /* invalid, vk maps to 0 (ZERO) */
    WGPUBlendFactor_Constant,
    WGPUBlendFactor_OneMinusConstant,
    /* WebGPU has no separate CONSTANT_ALPHA factor: when used for the color
     * component this uses the constant's rgb instead of its alpha */
    WGPUBlendFactor_Constant,
    WGPUBlendFactor_OneMinusConstant,
};

static const WGPUBlendOperation pgraph_blend_equation_wgpu_map[] = {
    WGPUBlendOperation_Subtract,
    WGPUBlendOperation_ReverseSubtract,
    WGPUBlendOperation_Add,
    WGPUBlendOperation_Min,
    WGPUBlendOperation_Max,
    WGPUBlendOperation_ReverseSubtract,
    WGPUBlendOperation_Add,
};

static const WGPUCompareFunction pgraph_compare_func_wgpu_map[] = {
    WGPUCompareFunction_Never,
    WGPUCompareFunction_Less,
    WGPUCompareFunction_Equal,
    WGPUCompareFunction_LessEqual,
    WGPUCompareFunction_Greater,
    WGPUCompareFunction_NotEqual,
    WGPUCompareFunction_GreaterEqual,
    WGPUCompareFunction_Always,
};

static const WGPUStencilOperation pgraph_stencil_op_wgpu_map[] = {
    WGPUStencilOperation_Keep, /* 0: vk maps to 0 (KEEP) */
    WGPUStencilOperation_Keep,
    WGPUStencilOperation_Zero,
    WGPUStencilOperation_Replace,
    WGPUStencilOperation_IncrementClamp,
    WGPUStencilOperation_DecrementClamp,
    WGPUStencilOperation_Invert,
    WGPUStencilOperation_IncrementWrap,
    WGPUStencilOperation_DecrementWrap,
};

/* ---- primitive conversion ---- */

typedef enum PrimOutput {
    PRIM_OUT_POINTS,
    PRIM_OUT_LINES,
    PRIM_OUT_TRIANGLES,
} PrimOutput;

typedef struct PrimConv {
    int primitive_mode;   /* enum ShaderPrimitiveMode */
    int polygon_mode;     /* enum ShaderPolygonMode (front) */
    bool first_vertex_is_provoking;
    bool smooth_shading;
    PrimOutput out;
    WGPUPrimitiveTopology topology;
} PrimConv;

static void get_prim_conv(PGRAPHState *pg, PrimConv *pc)
{
    uint32_t setupraster = pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER);
    uint32_t control_3 = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_3);

    pc->primitive_mode = pg->primitive_mode;
    pc->polygon_mode =
        GET_MASK(setupraster, NV_PGRAPH_SETUPRASTER_FRONTFACEMODE);
    pc->first_vertex_is_provoking =
        GET_MASK(control_3, NV_PGRAPH_CONTROL_3_PROVOKING_VERTEX) ==
        NV_PGRAPH_CONTROL_3_PROVOKING_VERTEX_FIRST;
    pc->smooth_shading = GET_MASK(control_3, NV_PGRAPH_CONTROL_3_SHADEMODE) ==
                         NV_PGRAPH_CONTROL_3_SHADEMODE_SMOOTH;

    switch (pc->primitive_mode) {
    case PRIM_TYPE_POINTS:
        pc->out = PRIM_OUT_POINTS;
        break;
    case PRIM_TYPE_LINES:
    case PRIM_TYPE_LINE_LOOP:
    case PRIM_TYPE_LINE_STRIP:
        pc->out = PRIM_OUT_LINES;
        break;
    default:
        if (pc->polygon_mode == POLY_MODE_LINE) {
            pc->out = PRIM_OUT_LINES;
        } else if (pc->polygon_mode == POLY_MODE_POINT) {
            pc->out = PRIM_OUT_POINTS;
        } else {
            pc->out = PRIM_OUT_TRIANGLES;
        }
        if (pc->polygon_mode != POLY_MODE_FILL) {
            static bool logged;
            if (!logged) {
                fprintf(stderr, "[wgpu] polygon mode %s emulated with "
                        "line/point lists (no culling, flat color per "
                        "segment)\n",
                        pc->polygon_mode == POLY_MODE_LINE ? "LINE" : "POINT");
                logged = true;
            }
        }
        break;
    }

    static const WGPUPrimitiveTopology topo[] = {
        [PRIM_OUT_POINTS] = WGPUPrimitiveTopology_PointList,
        [PRIM_OUT_LINES] = WGPUPrimitiveTopology_LineList,
        [PRIM_OUT_TRIANGLES] = WGPUPrimitiveTopology_TriangleList,
    };
    pc->topology = topo[pc->out];

    if (pc->out == PRIM_OUT_POINTS) {
        static bool logged;
        if (!logged) {
            fprintf(stderr, "[wgpu] WebGPU only rasterizes 1px points; "
                    "NV2A point size / point sprites are not emulated\n");
            logged = true;
        }
    }
}

typedef struct IndexGen {
    PGRAPHWgpuDrawState *ds;
    const PrimConv *pc;
    const uint32_t *elements; /* NULL: vertex k is base + k */
    uint32_t base;            /* subtracted from elements / added to k */
} IndexGen;

static inline uint32_t gen_vtx(const IndexGen *g, uint32_t k)
{
    return g->elements ? g->elements[k] - g->base : g->base + k;
}

static inline void emit1(IndexGen *g, uint32_t v)
{
    PGRAPHWgpuDrawState *ds = g->ds;
    assert(ds->num_indices < ds->indices_capacity);
    ds->indices[ds->num_indices++] = v;
}

/* Line with its provoking vertex p first */
static inline void emit_line(IndexGen *g, uint32_t p, uint32_t o)
{
    emit1(g, p);
    emit1(g, o);
}

/* NV2A line (a, b): provoking vertex is a if first_vertex_is_provoking,
 * else b (glsl/geom.c) */
static inline void emit_nv2a_line(IndexGen *g, uint32_t a, uint32_t b)
{
    if (g->pc->first_vertex_is_provoking) {
        emit_line(g, a, b);
    } else {
        emit_line(g, b, a);
    }
}

/* Triangle (p, b, c) in NV2A winding order, rotated so that the provoking
 * vertex p is first */
static void emit_tri(IndexGen *g, uint32_t p, uint32_t b, uint32_t c)
{
    switch (g->pc->out) {
    case PRIM_OUT_TRIANGLES:
        emit1(g, p);
        emit1(g, b);
        emit1(g, c);
        break;
    case PRIM_OUT_LINES:
        emit_line(g, p, b);
        emit_line(g, b, c);
        emit_line(g, c, p);
        break;
    case PRIM_OUT_POINTS:
        emit1(g, p);
        emit1(g, b);
        emit1(g, c);
        break;
    }
}

/* Upper bound of indices generated for n vertices */
static size_t max_indices_for(uint32_t n)
{
    return (size_t)n * 6 + 16;
}

/*
 * Convert one NV2A primitive run of n vertices. Provoking vertices follow
 * glsl/geom.c (which reorders GS input to first-vertex convention):
 *
 *   NV2A prim        output         per primitive (provoking first)
 *   POINTS           point-list     v
 *   LINES            line-list      (a,b) first-provoking, else (b,a)
 *   LINE_STRIP       line-list      segments (i,i+1), same rule
 *   LINE_LOOP        line-list      strip segments + closing (n-1,0)
 *   TRIANGLES        triangle-list  (a,b,c) first-provoking, else (c,a,b)
 *   TRIANGLE_STRIP   triangle-list  even i: (i,i+1,i+2) | (i+2,i,i+1)
 *                                   odd i:  (i,i+2,i+1) | (i+2,i+1,i)
 *   TRIANGLE_FAN     triangle-list  (i+1,i+2,0) first-provoking,
 *                                   else (i+2,0,i+1)
 *   POLYGON (fill)   triangle-list  (0,i+1,i+2) (provoking = vertex 0)
 *   QUADS            triangle-list  flat: (d,a,b),(d,b,c) (provoking = d)
 *                                   smooth: (a,b,c),(a,c,d) as the GS
 *   QUAD_STRIP       triangle-list  quad a,b,c,d = 2k..2k+3,
 *                                   flat: (d,c,a),(d,a,b) (provoking = d)
 *                                   smooth: (a,b,c),(c,b,d) as the GS
 * Polygon mode LINE emits the primitive edges as a line-list (quads/quad
 * strips/polygons: outline without diagonals), POINT emits the vertices.
 */
static void gen_prim_indices(IndexGen *g, uint32_t n)
{
    const PrimConv *pc = g->pc;
    PGRAPHWgpuDrawState *ds = g->ds;

    size_t need = ds->num_indices + max_indices_for(n);
    if (need > ds->indices_capacity) {
        ds->indices_capacity = MAX(need, ds->indices_capacity * 2);
        ds->indices = g_renew(uint32_t, ds->indices, ds->indices_capacity);
    }

#define V(k) gen_vtx(g, (k))
    switch (pc->primitive_mode) {
    case PRIM_TYPE_POINTS:
        for (uint32_t k = 0; k < n; k++) {
            emit1(g, V(k));
        }
        break;
    case PRIM_TYPE_LINES:
        for (uint32_t k = 0; k + 1 < n; k += 2) {
            emit_nv2a_line(g, V(k), V(k + 1));
        }
        break;
    case PRIM_TYPE_LINE_STRIP:
    case PRIM_TYPE_LINE_LOOP:
        for (uint32_t k = 0; k + 1 < n; k++) {
            emit_nv2a_line(g, V(k), V(k + 1));
        }
        if (pc->primitive_mode == PRIM_TYPE_LINE_LOOP && n > 2) {
            emit_nv2a_line(g, V(n - 1), V(0));
        }
        break;
    case PRIM_TYPE_TRIANGLES:
        for (uint32_t k = 0; k + 2 < n; k += 3) {
            if (pc->first_vertex_is_provoking) {
                emit_tri(g, V(k), V(k + 1), V(k + 2));
            } else {
                emit_tri(g, V(k + 2), V(k), V(k + 1));
            }
        }
        break;
    case PRIM_TYPE_TRIANGLE_STRIP:
        for (uint32_t i = 0; i + 2 < n; i++) {
            /* first-vertex-convention order, winding preserved */
            uint32_t o0 = V(i);
            uint32_t o1 = (i & 1) ? V(i + 2) : V(i + 1);
            uint32_t o2 = (i & 1) ? V(i + 1) : V(i + 2);
            if (pc->first_vertex_is_provoking) {
                emit_tri(g, o0, o1, o2);
            } else if (i & 1) {
                emit_tri(g, o1, o2, o0);
            } else {
                emit_tri(g, o2, o0, o1);
            }
        }
        break;
    case PRIM_TYPE_TRIANGLE_FAN:
        for (uint32_t i = 0; i + 2 < n; i++) {
            uint32_t o0 = V(i + 1), o1 = V(i + 2), o2 = V(0);
            if (pc->first_vertex_is_provoking) {
                emit_tri(g, o0, o1, o2);
            } else {
                emit_tri(g, o1, o2, o0);
            }
        }
        break;
    case PRIM_TYPE_POLYGON:
        if (pc->out == PRIM_OUT_TRIANGLES) {
            for (uint32_t i = 0; i + 2 < n; i++) {
                emit_tri(g, V(0), V(i + 1), V(i + 2));
            }
        } else if (pc->out == PRIM_OUT_LINES) {
            for (uint32_t k = 0; k + 1 < n; k++) {
                emit_line(g, V(k), V(k + 1));
            }
            if (n > 2) {
                emit_line(g, V(n - 1), V(0));
            }
        } else {
            for (uint32_t k = 0; k < n; k++) {
                emit1(g, V(k));
            }
        }
        break;
    case PRIM_TYPE_QUADS:
        for (uint32_t k = 0; k + 3 < n; k += 4) {
            uint32_t a = V(k), b = V(k + 1), c = V(k + 2), d = V(k + 3);
            if (pc->out == PRIM_OUT_TRIANGLES) {
                if (pc->smooth_shading) {
                    emit_tri(g, a, b, c);
                    emit_tri(g, a, c, d);
                } else {
                    emit_tri(g, d, a, b);
                    emit_tri(g, d, b, c);
                }
            } else if (pc->out == PRIM_OUT_LINES) {
                emit_line(g, a, b);
                emit_line(g, b, c);
                emit_line(g, c, d);
                emit_line(g, d, a);
            } else {
                emit1(g, a);
                emit1(g, b);
                emit1(g, c);
                emit1(g, d);
            }
        }
        break;
    case PRIM_TYPE_QUAD_STRIP:
        for (uint32_t k = 0; k + 3 < n; k += 2) {
            uint32_t a = V(k), b = V(k + 1), c = V(k + 2), d = V(k + 3);
            if (pc->out == PRIM_OUT_TRIANGLES) {
                if (pc->smooth_shading) {
                    emit_tri(g, a, b, c);
                    emit_tri(g, c, b, d);
                } else {
                    emit_tri(g, d, c, a);
                    emit_tri(g, d, a, b);
                }
            } else if (pc->out == PRIM_OUT_LINES) {
                emit_line(g, a, b);
                emit_line(g, b, d);
                emit_line(g, d, c);
                emit_line(g, c, a);
            } else {
                emit1(g, a);
                emit1(g, b);
                emit1(g, c);
                emit1(g, d);
            }
        }
        break;
    default:
        assert(!"Invalid primitive_mode");
        break;
    }
#undef V
}

/* ---- pipeline cache ---- */

static void pipeline_cache_entry_init(Lru *lru, LruNode *node,
                                      const void *state)
{
    WgpuPipelineBinding *snode = container_of(node, WgpuPipelineBinding, node);
    snode->pipeline = NULL;
    snode->draw_time = 0;
    snode->cull_all = false;
    snode->stencil_ref = 0;
    snode->blend_constant = (WGPUColor){ 0, 0, 0, 0 };
}

static void pipeline_cache_entry_post_evict(Lru *lru, LruNode *node)
{
    WgpuPipelineBinding *snode = container_of(node, WgpuPipelineBinding, node);
    PGRAPHWgpuDrawState *ds =
        container_of(lru, PGRAPHWgpuDrawState, pipeline_cache);

    /* WebGPU objects are reference counted: an encoder keeps a pipeline it
     * recorded alive, so eviction while in use is safe. */
    if (snode->pipeline) {
        wgpuRenderPipelineRelease(snode->pipeline);
        snode->pipeline = NULL;
    }
    if (ds->pipeline_binding == snode) {
        ds->pipeline_binding = NULL;
    }
    if (ds->pass_pipeline == snode) {
        ds->pass_pipeline = NULL;
    }
}

static bool pipeline_cache_entry_compare(Lru *lru, LruNode *node,
                                         const void *key)
{
    WgpuPipelineBinding *snode = container_of(node, WgpuPipelineBinding, node);
    return memcmp(&snode->key, key, sizeof(WgpuPipelineKey));
}

static void init_pipeline_cache(PGRAPHState *pg)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;
    PGRAPHWgpuDrawState *ds = &r->draw;

    const size_t pipeline_cache_size = 2048;
    lru_init(&ds->pipeline_cache);
    ds->pipeline_cache_entries =
        g_malloc0_n(pipeline_cache_size, sizeof(WgpuPipelineBinding));
    for (int i = 0; i < pipeline_cache_size; i++) {
        lru_add_free(&ds->pipeline_cache,
                     &ds->pipeline_cache_entries[i].node);
    }

    ds->pipeline_cache.init_node = pipeline_cache_entry_init;
    ds->pipeline_cache.compare_nodes = pipeline_cache_entry_compare;
    ds->pipeline_cache.post_node_evict = pipeline_cache_entry_post_evict;
}

static void finalize_pipeline_cache(PGRAPHState *pg)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;
    PGRAPHWgpuDrawState *ds = &r->draw;

    lru_flush(&ds->pipeline_cache);
    g_free(ds->pipeline_cache_entries);
    ds->pipeline_cache_entries = NULL;
    ds->pipeline_binding = NULL;
    ds->pass_pipeline = NULL;
}

/* Full-screen triangle; position (incl. clear depth in z) comes from a
 * vertex buffer, color from the blend constant (src=Constant, dst=Zero) */
static const char clear_wgsl[] =
    "struct VOut { @builtin(position) pos: vec4f };\n"
    "@vertex fn vs(@location(0) p: vec4f) -> VOut {\n"
    "    var o: VOut;\n"
    "    o.pos = p;\n"
    "    return o;\n"
    "}\n"
    "@fragment fn fs() -> @location(0) vec4f {\n"
    "    return vec4f(1.0);\n"
    "}\n";

void pgraph_wgpu_init_pipelines(PGRAPHState *pg)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;
    PGRAPHWgpuDrawState *ds = &r->draw;

    init_pipeline_cache(pg);
    ds->clear_module = pgraph_wgpu_create_wgsl_module(r, "nv2a-clear",
                                                      clear_wgsl);
    ds->empty_pipeline_layout = wgpuDeviceCreatePipelineLayout(
        r->device, &(WGPUPipelineLayoutDescriptor){
                       .label = { "nv2a-clear", WGPU_STRLEN } });
}

void pgraph_wgpu_finalize_pipelines(PGRAPHState *pg)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;
    PGRAPHWgpuDrawState *ds = &r->draw;

    finalize_pipeline_cache(pg);
    if (ds->clear_module) {
        wgpuShaderModuleRelease(ds->clear_module);
        ds->clear_module = NULL;
    }
    if (ds->empty_pipeline_layout) {
        wgpuPipelineLayoutRelease(ds->empty_pipeline_layout);
        ds->empty_pipeline_layout = NULL;
    }
}

static void init_render_pass_state(PGRAPHState *pg, WgpuPipelineKey *key)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;

    key->color_format = r->color_binding ? r->color_binding->host_fmt.format :
                                           WGPUTextureFormat_Undefined;
    /* no zeta surface -> the pass gets a scratch Depth32Float attachment
     * (WebGPU needs a depth attachment whenever the FS writes frag_depth,
     * which xemu's pixel shaders always do) */
    key->zeta_format = r->zeta_binding ? r->zeta_binding->host_fmt.format :
                                         WGPUTextureFormat_Depth32Float;
}

static void create_clear_pipeline(PGRAPHState *pg)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;
    PGRAPHWgpuDrawState *ds = &r->draw;

    WgpuPipelineKey key;
    memset(&key, 0, sizeof(key));
    key.clear = true;
    init_render_pass_state(pg, &key);
    key.topology = WGPUPrimitiveTopology_TriangleList;
    key.regs[0] = ds->clear_parameter;

    uint64_t hash = fast_hash((void *)&key, sizeof(key));
    LruNode *node = lru_lookup(&ds->pipeline_cache, hash, &key);
    WgpuPipelineBinding *snode = container_of(node, WgpuPipelineBinding, node);

    if (snode->pipeline) {
        ds->pipeline_binding_changed = ds->pipeline_binding != snode;
        ds->pipeline_binding = snode;
        return;
    }

    nv2a_profile_inc_counter(NV2A_PROF_PIPELINE_GEN);
    memcpy(&snode->key, &key, sizeof(key));

    uint32_t param = ds->clear_parameter;

    WGPUColorWriteMask write_mask = WGPUColorWriteMask_None;
    if (param & NV097_CLEAR_SURFACE_R)
        write_mask |= WGPUColorWriteMask_Red;
    if (param & NV097_CLEAR_SURFACE_G)
        write_mask |= WGPUColorWriteMask_Green;
    if (param & NV097_CLEAR_SURFACE_B)
        write_mask |= WGPUColorWriteMask_Blue;
    if (param & NV097_CLEAR_SURFACE_A)
        write_mask |= WGPUColorWriteMask_Alpha;

    WGPUBlendState blend = {
        .color = { .operation = WGPUBlendOperation_Add,
                   .srcFactor = WGPUBlendFactor_Constant,
                   .dstFactor = WGPUBlendFactor_Zero },
        .alpha = { .operation = WGPUBlendOperation_Add,
                   .srcFactor = WGPUBlendFactor_Constant,
                   .dstFactor = WGPUBlendFactor_Zero },
    };
    WGPUColorTargetState target = {
        .format = key.color_format,
        .blend = &blend,
        .writeMask = write_mask,
    };
    WGPUFragmentState fragment = {
        .module = ds->clear_module,
        .entryPoint = { "fs", WGPU_STRLEN },
        .targetCount = 1,
        .targets = &target,
    };

    WGPUDepthStencilState depth_stencil = {
        .format = key.zeta_format,
        .depthWriteEnabled = WGPUOptionalBool_False,
        .depthCompare = WGPUCompareFunction_Always,
        .stencilFront = { .compare = WGPUCompareFunction_Always,
                          .failOp = WGPUStencilOperation_Keep,
                          .depthFailOp = WGPUStencilOperation_Keep,
                          .passOp = WGPUStencilOperation_Keep },
        .stencilReadMask = 0xff,
        .stencilWriteMask = 0xff,
    };
    if (r->zeta_binding) {
        if ((param & NV097_CLEAR_SURFACE_Z) &&
            r->zeta_binding->host_fmt.depth) {
            depth_stencil.depthWriteEnabled = WGPUOptionalBool_True;
        }
        if ((param & NV097_CLEAR_SURFACE_STENCIL) &&
            r->zeta_binding->host_fmt.stencil) {
            depth_stencil.stencilFront = (WGPUStencilFaceState){
                .compare = WGPUCompareFunction_Always,
                .failOp = WGPUStencilOperation_Replace,
                .depthFailOp = WGPUStencilOperation_Replace,
                .passOp = WGPUStencilOperation_Replace,
            };
        }
    }
    depth_stencil.stencilBack = depth_stencil.stencilFront;

    WGPUVertexAttribute attr = {
        .format = WGPUVertexFormat_Float32x4,
        .offset = 0,
        .shaderLocation = 0,
    };
    WGPUVertexBufferLayout vbl = {
        .stepMode = WGPUVertexStepMode_Vertex,
        .arrayStride = 4 * sizeof(float),
        .attributeCount = 1,
        .attributes = &attr,
    };

    WGPURenderPipelineDescriptor desc = {
        .label = { "nv2a-clear", WGPU_STRLEN },
        .layout = ds->empty_pipeline_layout,
        .vertex = { .module = ds->clear_module,
                    .entryPoint = { "vs", WGPU_STRLEN },
                    .bufferCount = 1,
                    .buffers = &vbl },
        .primitive = { .topology = WGPUPrimitiveTopology_TriangleList,
                       .frontFace = WGPUFrontFace_CCW,
                       .cullMode = WGPUCullMode_None },
        .depthStencil = &depth_stencil,
        .multisample = { .count = 1, .mask = ~0u },
        .fragment = r->color_binding ? &fragment : NULL,
    };

    XSTAT_INC(n_pipeline_gen);
    snode->pipeline = wgpuDeviceCreateRenderPipeline(r->device, &desc);
    snode->draw_time = pg->draw_time;
    snode->cull_all = false;

    ds->pipeline_binding = snode;
    ds->pipeline_binding_changed = true;
}

static bool check_render_pass_dirty(PGRAPHState *pg)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;
    assert(r->draw.pipeline_binding);

    WgpuPipelineKey key;
    memset(&key, 0, sizeof(key));
    init_render_pass_state(pg, &key);

    return key.color_format != r->draw.pipeline_binding->key.color_format ||
           key.zeta_format != r->draw.pipeline_binding->key.zeta_format;
}

// Quickly check for any state changes that would require more analysis
static bool check_pipeline_dirty(PGRAPHState *pg, uint32_t topology)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;
    PGRAPHWgpuDrawState *ds = &r->draw;

    if (!ds->pipeline_binding || ds->pipeline_binding->key.clear ||
        r->shader_bindings_changed || r->texture_bindings_changed ||
        check_render_pass_dirty(pg)) {
        return true;
    }

    const unsigned int regs[] = {
        NV_PGRAPH_BLEND,       NV_PGRAPH_BLENDCOLOR,  NV_PGRAPH_CONTROL_0,
        NV_PGRAPH_CONTROL_1,   NV_PGRAPH_CONTROL_2,   NV_PGRAPH_CONTROL_3,
        NV_PGRAPH_SETUPRASTER, NV_PGRAPH_ZOFFSETBIAS, NV_PGRAPH_ZOFFSETFACTOR,
    };

    for (int i = 0; i < ARRAY_SIZE(regs); i++) {
        if (pgraph_is_reg_dirty(pg, regs[i])) {
            return true;
        }
    }

    if (ds->pipeline_binding->key.topology != topology ||
        memcmp(&ds->vertex_layout, &ds->pipeline_binding->key.vertex,
               sizeof(ds->vertex_layout))) {
        return true;
    }

    nv2a_profile_inc_counter(NV2A_PROF_PIPELINE_NOTDIRTY);

    return false;
}

static void init_pipeline_key(PGRAPHState *pg, WgpuPipelineKey *key,
                              uint32_t topology)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;

    memset(key, 0, sizeof(*key));
    init_render_pass_state(pg, key);
    memcpy(&key->shader_state, &r->shaders.shader_binding->state,
           sizeof(ShaderState));
    memcpy(&key->vertex, &r->draw.vertex_layout, sizeof(key->vertex));
    key->topology = topology;
    QEMU_BUILD_BUG_ON(sizeof(ShaderLayoutKey) > sizeof(key->layout_key));
    if (r->shaders.shader_binding->layout) {
        memcpy(key->layout_key, &r->shaders.shader_binding->layout->key,
               sizeof(ShaderLayoutKey));
    }
    key->pipeline_layout =
        (uintptr_t)r->shaders.shader_binding->pipeline_layout;

    // FIXME: Register masking
    const int regs[] = {
        NV_PGRAPH_BLEND,       NV_PGRAPH_BLENDCOLOR,  NV_PGRAPH_CONTROL_0,
        NV_PGRAPH_CONTROL_1,   NV_PGRAPH_CONTROL_2,   NV_PGRAPH_CONTROL_3,
        NV_PGRAPH_SETUPRASTER, NV_PGRAPH_ZOFFSETBIAS, NV_PGRAPH_ZOFFSETFACTOR,
    };
    assert(ARRAY_SIZE(regs) == ARRAY_SIZE(key->regs));
    for (int i = 0; i < ARRAY_SIZE(regs); i++) {
        key->regs[i] = pgraph_reg_r(pg, regs[i]);
    }
}

static void create_pipeline(PGRAPHState *pg, uint32_t topology)
{
    NV2AState *d = container_of(pg, NV2AState, pgraph);
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;
    PGRAPHWgpuDrawState *ds = &r->draw;

    XPHASE_SET(XPHASE_GPU, "draw:textures");
    pgraph_wgpu_bind_textures(d);
    XPHASE_SET(XPHASE_GPU, "draw:shaders");
    pgraph_wgpu_bind_shaders(pg);
    XPHASE_SET(XPHASE_GPU, "draw:pipeline");

    ShaderBinding *sb = r->shaders.shader_binding;
    if (!sb || !sb->vsh_module || !sb->psh_module || !sb->pipeline_layout) {
        pgraph_clear_dirty_reg_map(pg);
        ds->pipeline_binding = NULL;
        return;
    }

    bool pipeline_dirty = check_pipeline_dirty(pg, topology);

    pgraph_clear_dirty_reg_map(pg);

    if (ds->pipeline_binding && !pipeline_dirty) {
        return;
    }

    WgpuPipelineKey key;
    init_pipeline_key(pg, &key, topology);
    uint64_t hash = fast_hash((void *)&key, sizeof(key));

    LruNode *node = lru_lookup(&ds->pipeline_cache, hash, &key);
    WgpuPipelineBinding *snode = container_of(node, WgpuPipelineBinding, node);
    if (snode->pipeline) {
        ds->pipeline_binding_changed = ds->pipeline_binding != snode;
        ds->pipeline_binding = snode;
        return;
    }

    nv2a_profile_inc_counter(NV2A_PROF_PIPELINE_GEN);

    memcpy(&snode->key, &key, sizeof(key));

    uint32_t control_0 = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0);
    uint32_t control_1 = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1);
    uint32_t control_2 = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_2);
    uint32_t setupraster = pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER);
    uint32_t blend_reg = pgraph_reg_r(pg, NV_PGRAPH_BLEND);
    bool depth_test = control_0 & NV_PGRAPH_CONTROL_0_ZENABLE;
    bool depth_write = !!(control_0 & NV_PGRAPH_CONTROL_0_ZWRITEENABLE);
    bool stencil_test = control_1 & NV_PGRAPH_CONTROL_1_STENCIL_TEST_ENABLE;

    /* vertex input */
    WGPUVertexAttribute attrs[NV2A_VERTEXSHADER_ATTRIBUTES]
                            [NV2A_VERTEXSHADER_ATTRIBUTES];
    WGPUVertexBufferLayout buffers[NV2A_VERTEXSHADER_ATTRIBUTES];
    memset(buffers, 0, sizeof(buffers));
    for (uint32_t b = 0; b < key.vertex.num_buffers; b++) {
        buffers[b].stepMode = WGPUVertexStepMode_Vertex;
        buffers[b].arrayStride = key.vertex.strides[b];
        buffers[b].attributes = attrs[b];
    }
    for (uint32_t i = 0; i < key.vertex.num_attrs; i++) {
        const WgpuVertexAttrKey *a = &key.vertex.attrs[i];
        WGPUVertexBufferLayout *b = &buffers[a->buffer];
        attrs[a->buffer][b->attributeCount++] = (WGPUVertexAttribute){
            .format = a->format,
            .offset = a->offset,
            .shaderLocation = a->location,
        };
    }

    /* rasterizer */
    WGPUPrimitiveState primitive = {
        .topology = topology,
        .stripIndexFormat = WGPUIndexFormat_Undefined,
        .frontFace = (setupraster & NV_PGRAPH_SETUPRASTER_FRONTFACE) ?
                         WGPUFrontFace_CCW :
                         WGPUFrontFace_CW,
        .cullMode = WGPUCullMode_None,
        /* vk: depthClampEnable = VK_TRUE */
        .unclippedDepth = r->has_unclipped_depth,
    };

    snode->cull_all = false;
    if (setupraster & NV_PGRAPH_SETUPRASTER_CULLENABLE) {
        uint32_t cull_face =
            GET_MASK(setupraster, NV_PGRAPH_SETUPRASTER_CULLCTRL);
        switch (cull_face) {
        case 1:
            primitive.cullMode = WGPUCullMode_Front;
            break;
        case 2:
            primitive.cullMode = WGPUCullMode_Back;
            break;
        case 3:
            /* FRONT_AND_BACK: polygons are never drawn */
            snode->cull_all = true;
            break;
        default:
            break;
        }
    }

    /* depth / stencil (depth bias is applied by the pixel shader, as in the
     * Vulkan renderer, see glsl/psh.c ZOFFSET handling) */
    WGPUDepthStencilState depth_stencil = {
        .format = key.zeta_format,
        .depthWriteEnabled = WGPUOptionalBool_False,
        .depthCompare = WGPUCompareFunction_Always,
        .stencilFront = { .compare = WGPUCompareFunction_Always,
                          .failOp = WGPUStencilOperation_Keep,
                          .depthFailOp = WGPUStencilOperation_Keep,
                          .passOp = WGPUStencilOperation_Keep },
        .stencilReadMask = 0xff,
        .stencilWriteMask = 0xff,
    };

    if (depth_test && r->zeta_binding) {
        uint32_t depth_func = GET_MASK(control_0, NV_PGRAPH_CONTROL_0_ZFUNC);
        assert(depth_func < ARRAY_SIZE(pgraph_compare_func_wgpu_map));
        depth_stencil.depthCompare = pgraph_compare_func_wgpu_map[depth_func];
        /* Vulkan: depth writes only happen with the depth test enabled */
        if (depth_write) {
            depth_stencil.depthWriteEnabled = WGPUOptionalBool_True;
        }
    }

    snode->stencil_ref = 0;
    if (stencil_test && r->zeta_binding && r->zeta_binding->host_fmt.stencil) {
        uint32_t stencil_func =
            GET_MASK(control_1, NV_PGRAPH_CONTROL_1_STENCIL_FUNC);
        uint32_t stencil_ref =
            GET_MASK(control_1, NV_PGRAPH_CONTROL_1_STENCIL_REF);
        uint32_t mask_read =
            GET_MASK(control_1, NV_PGRAPH_CONTROL_1_STENCIL_MASK_READ);
        uint32_t mask_write =
            GET_MASK(control_1, NV_PGRAPH_CONTROL_1_STENCIL_MASK_WRITE);
        uint32_t op_fail =
            GET_MASK(control_2, NV_PGRAPH_CONTROL_2_STENCIL_OP_FAIL);
        uint32_t op_zfail =
            GET_MASK(control_2, NV_PGRAPH_CONTROL_2_STENCIL_OP_ZFAIL);
        uint32_t op_zpass =
            GET_MASK(control_2, NV_PGRAPH_CONTROL_2_STENCIL_OP_ZPASS);

        assert(stencil_func < ARRAY_SIZE(pgraph_compare_func_wgpu_map));
        assert(op_fail < ARRAY_SIZE(pgraph_stencil_op_wgpu_map));
        assert(op_zfail < ARRAY_SIZE(pgraph_stencil_op_wgpu_map));
        assert(op_zpass < ARRAY_SIZE(pgraph_stencil_op_wgpu_map));

        depth_stencil.stencilFront = (WGPUStencilFaceState){
            .compare = pgraph_compare_func_wgpu_map[stencil_func],
            .failOp = pgraph_stencil_op_wgpu_map[op_fail],
            .depthFailOp = pgraph_stencil_op_wgpu_map[op_zfail],
            .passOp = pgraph_stencil_op_wgpu_map[op_zpass],
        };
        depth_stencil.stencilReadMask = mask_read;
        depth_stencil.stencilWriteMask = mask_write;
        snode->stencil_ref = stencil_ref;
    }
    depth_stencil.stencilBack = depth_stencil.stencilFront;

    /* color */
    WGPUColorWriteMask write_mask = WGPUColorWriteMask_None;
    if (control_0 & NV_PGRAPH_CONTROL_0_RED_WRITE_ENABLE)
        write_mask |= WGPUColorWriteMask_Red;
    if (control_0 & NV_PGRAPH_CONTROL_0_GREEN_WRITE_ENABLE)
        write_mask |= WGPUColorWriteMask_Green;
    if (control_0 & NV_PGRAPH_CONTROL_0_BLUE_WRITE_ENABLE)
        write_mask |= WGPUColorWriteMask_Blue;
    if (control_0 & NV_PGRAPH_CONTROL_0_ALPHA_WRITE_ENABLE)
        write_mask |= WGPUColorWriteMask_Alpha;

    WGPUBlendState blend;
    WGPUColorTargetState target = {
        .format = key.color_format,
        .writeMask = write_mask,
    };
    snode->blend_constant = (WGPUColor){ 0, 0, 0, 0 };

    if (blend_reg & NV_PGRAPH_BLEND_EN) {
        uint32_t sfactor = GET_MASK(blend_reg, NV_PGRAPH_BLEND_SFACTOR);
        uint32_t dfactor = GET_MASK(blend_reg, NV_PGRAPH_BLEND_DFACTOR);
        uint32_t equation = GET_MASK(blend_reg, NV_PGRAPH_BLEND_EQN);
        assert(sfactor < ARRAY_SIZE(pgraph_blend_factor_wgpu_map));
        assert(dfactor < ARRAY_SIZE(pgraph_blend_factor_wgpu_map));
        assert(equation < ARRAY_SIZE(pgraph_blend_equation_wgpu_map));

        WGPUBlendComponent comp = {
            .operation = pgraph_blend_equation_wgpu_map[equation],
            .srcFactor = pgraph_blend_factor_wgpu_map[sfactor],
            .dstFactor = pgraph_blend_factor_wgpu_map[dfactor],
        };
        if (comp.operation == WGPUBlendOperation_Min ||
            comp.operation == WGPUBlendOperation_Max) {
            /* WebGPU requires factors One for min/max (Vulkan ignores
             * them) */
            comp.srcFactor = WGPUBlendFactor_One;
            comp.dstFactor = WGPUBlendFactor_One;
        }
        blend.color = comp;
        blend.alpha = comp;
        target.blend = &blend;

        float blend_constant[4];
        uint32_t blend_color = pgraph_reg_r(pg, NV_PGRAPH_BLENDCOLOR);
        pgraph_argb_pack32_to_rgba_float(blend_color, blend_constant);
        snode->blend_constant = (WGPUColor){
            blend_constant[0], blend_constant[1], blend_constant[2],
            blend_constant[3],
        };

        /* NV2A CONSTANT_ALPHA / ONE_MINUS_CONSTANT_ALPHA (14, 15) map to
         * WebGPU Constant / OneMinusConstant: splat the constant's alpha
         * so the color component uses it, unless CONSTANT_COLOR factors
         * (12, 13) need the rgb at the same time. */
        bool uses_const_alpha = sfactor == 14 || sfactor == 15 ||
                                dfactor == 14 || dfactor == 15;
        bool uses_const_color = sfactor == 12 || sfactor == 13 ||
                                dfactor == 12 || dfactor == 13;
        if (uses_const_alpha && !uses_const_color) {
            double a = blend_constant[3];
            snode->blend_constant = (WGPUColor){ a, a, a, a };
        } else if (uses_const_alpha) {
            static bool logged;
            if (!logged) {
                fprintf(stderr, "[wgpu] blend with both CONSTANT_COLOR and "
                        "CONSTANT_ALPHA factors is approximated\n");
                logged = true;
            }
        }
    }

    // FIXME: Dither
    // FIXME: point size (WebGPU points are always 1px)
    // FIXME: Edge Antialiasing
    // FIXME: Line width (WebGPU lines are always 1px)

    WGPUFragmentState fragment = {
        .module = sb->psh_module,
        .entryPoint = { "main", WGPU_STRLEN },
        .targetCount = r->color_binding ? 1 : 0,
        .targets = r->color_binding ? &target : NULL,
    };

    WGPURenderPipelineDescriptor desc = {
        .label = { "nv2a-draw", WGPU_STRLEN },
        .layout = sb->pipeline_layout,
        .vertex = { .module = sb->vsh_module,
                    .entryPoint = { "main", WGPU_STRLEN },
                    .bufferCount = key.vertex.num_buffers,
                    .buffers = buffers },
        .primitive = primitive,
        .depthStencil = &depth_stencil,
        .multisample = { .count = 1, .mask = ~0u },
        .fragment = &fragment,
    };

    snode->pipeline = wgpuDeviceCreateRenderPipeline(r->device, &desc);
    snode->draw_time = pg->draw_time;

    ds->pipeline_binding = snode;
    ds->pipeline_binding_changed = true;
}

/* ---- draw sequencing ---- */

static bool ensure_buffer_space(PGRAPHState *pg, int index, size_t size)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;
    WgpuStorageBuffer *b = &r->draw.storage_buffers[index];

    if (ROUND_UP(size, 4) > b->buffer_size) {
        static bool logged;
        if (!logged) {
            fprintf(stderr, "[wgpu] draw needs %zu bytes in %s (size %zu), "
                    "skipping\n", size, b->label, b->buffer_size);
            logged = true;
        }
        return false;
    }
    if (!pgraph_wgpu_buffer_has_space_for(pg, index, size, 4)) {
        pgraph_wgpu_finish(pg, WGPU_FINISH_REASON_NEED_BUFFER_SPACE);
    }
    return true;
}

/*
 * Resolve pipeline / uniforms and make sure an encoder is open. Anything
 * that may finish (and thereby reset the append-only buffers) happens here,
 * before the draw's own buffer appends. Returns false if nothing can be
 * drawn.
 */
static bool begin_pre_draw(PGRAPHState *pg, uint32_t topology)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;
    PGRAPHWgpuDrawState *ds = &r->draw;

    assert(r->color_binding || r->zeta_binding);
    assert(!r->color_binding || r->color_binding->initialized);
    assert(!r->zeta_binding || r->zeta_binding->initialized);

    if (pg->clearing) {
        create_clear_pipeline(pg);
    } else {
        XPHASE_SET(XPHASE_GPU, "draw:pipeline");
        create_pipeline(pg, topology);
        XPHASE_SET(XPHASE_GPU, "draw");
    }
    if (!ds->pipeline_binding || !ds->pipeline_binding->pipeline) {
        return false;
    }

    /*
     * Reserve occlusion query capacity first: finishing resets the uniform
     * ring, which would let later uploads overwrite this draw's uniforms
     * before its command buffer is submitted.
     */
    if (!pg->clearing && pg->zpass_pixel_count_enable &&
        ds->in_command_buffer &&
        (ds->num_queries_in_flight >= WGPU_MAX_QUERIES_IN_FLIGHT ||
         ds->num_gpu_queries >= WGPU_MAX_GPU_QUERIES)) {
        pgraph_wgpu_finish(pg, WGPU_FINISH_REASON_NEED_BUFFER_SPACE);
    }

    if (!pg->clearing) {
        /* vk: pgraph_vk_update_descriptor_sets */
        XPHASE_SET(XPHASE_GPU, "draw:bindgroup");
        ds->bind_group = pgraph_wgpu_update_bind_group(pg);
        XPHASE_SET(XPHASE_GPU, "draw");
        if (!ds->bind_group) {
            return false;
        }
        /* the pipeline must still be cached (the shaders module could only
         * have finished, which does not evict) */
        assert(ds->pipeline_binding);
    }

    pgraph_wgpu_ensure_command_buffer(pg);

    bool attachments_changed =
        ds->in_render_pass &&
        (ds->pass_color_view !=
             (r->color_binding ? r->color_binding->view : NULL) ||
         ds->pass_zeta_view !=
             (r->zeta_binding ? r->zeta_binding->view : NULL));

    if (r->framebuffer_dirty || attachments_changed) {
        pgraph_wgpu_ensure_not_in_render_pass(pg);
        r->framebuffer_dirty = false;
    }

    return true;
}

static void begin_draw(PGRAPHState *pg)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;
    PGRAPHWgpuDrawState *ds = &r->draw;

    assert(ds->in_command_buffer);

    // Visibility testing
    if (!pg->clearing && pg->zpass_pixel_count_enable) {
        if (ds->new_query_needed && ds->query_in_flight) {
            pgraph_wgpu_end_query(pg);
        }
        if (!ds->query_in_flight) {
            pgraph_wgpu_begin_query(pg);
        }
    } else if (ds->query_in_flight) {
        pgraph_wgpu_end_query(pg);
    }

    if (!ds->in_render_pass) {
        pgraph_wgpu_begin_render_pass(pg, NULL, NULL, NULL);
    }

    WgpuPipelineBinding *pb = ds->pipeline_binding;
    if (ds->pipeline_binding_changed || ds->pass_pipeline != pb) {
        nv2a_profile_inc_counter(NV2A_PROF_PIPELINE_BIND);
        wgpuRenderPassEncoderSetPipeline(ds->pass, pb->pipeline);
        wgpuRenderPassEncoderSetBlendConstant(ds->pass, &pb->blend_constant);
        wgpuRenderPassEncoderSetStencilReference(ds->pass, pb->stencil_ref);
        ds->pass_pipeline = pb;
        ds->pipeline_binding_changed = false;
    }
    pb->draw_time = pg->draw_time;

    unsigned int vp_width = pg->surface_binding_dim.width,
                 vp_height = pg->surface_binding_dim.height;
    pgraph_apply_scaling_factor(pg, &vp_width, &vp_height);
    /* WebGPU validates the viewport against the attachment size */
    vp_width = MAX(1, MIN(vp_width, ds->pass_width));
    vp_height = MAX(1, MIN(vp_height, ds->pass_height));
    if (ds->pass_vp[0] != vp_width || ds->pass_vp[1] != vp_height) {
        wgpuRenderPassEncoderSetViewport(ds->pass, 0, 0, vp_width, vp_height,
                                         0.0f, 1.0f);
        ds->pass_vp[0] = vp_width;
        ds->pass_vp[1] = vp_height;
    }

    /* Surface clip */
    /* FIXME: Consider moving to PSH w/ window clip */
    unsigned int xmin = pg->surface_shape.clip_x,
                 ymin = pg->surface_shape.clip_y;

    unsigned int scissor_width = pg->surface_shape.clip_width,
                 scissor_height = pg->surface_shape.clip_height;

    pgraph_apply_anti_aliasing_factor(pg, &xmin, &ymin);
    pgraph_apply_anti_aliasing_factor(pg, &scissor_width, &scissor_height);

    pgraph_apply_scaling_factor(pg, &xmin, &ymin);
    pgraph_apply_scaling_factor(pg, &scissor_width, &scissor_height);

    xmin = MIN(xmin, ds->pass_width);
    ymin = MIN(ymin, ds->pass_height);
    scissor_width = MIN(scissor_width, ds->pass_width - xmin);
    scissor_height = MIN(scissor_height, ds->pass_height - ymin);
    if (ds->pass_sc[0] != xmin || ds->pass_sc[1] != ymin ||
        ds->pass_sc[2] != scissor_width || ds->pass_sc[3] != scissor_height) {
        wgpuRenderPassEncoderSetScissorRect(ds->pass, xmin, ymin,
                                            scissor_width, scissor_height);
        ds->pass_sc[0] = xmin;
        ds->pass_sc[1] = ymin;
        ds->pass_sc[2] = scissor_width;
        ds->pass_sc[3] = scissor_height;
    }

    if (!pg->clearing) {
        uint32_t off[2];
        pgraph_wgpu_uniform_offsets(pg, off);
        uint32_t gen = r->shaders.bind_group_gen;
        if (ds->pass_bg_gen != gen ||
            memcmp(ds->pass_bg_off, off, sizeof(off))) {
            wgpuRenderPassEncoderSetBindGroup(ds->pass, 0, ds->bind_group, 2,
                                              off);
            ds->pass_bg_gen = gen;
            memcpy(ds->pass_bg_off, off, sizeof(off));
        }
        pgraph_wgpu_begin_gpu_query_if_needed(pg);
    }

    ds->in_draw = true;
}

static void end_draw(PGRAPHState *pg)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;

    assert(r->draw.in_command_buffer);
    assert(r->draw.in_render_pass);

    r->draw.in_draw = false;
}

void pgraph_wgpu_draw_begin(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;

    NV2A_DPRINTF("NV097_SET_BEGIN_END: 0x%x\n", d->pgraph.primitive_mode);

    uint32_t control_0 = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0);
    bool mask_alpha = control_0 & NV_PGRAPH_CONTROL_0_ALPHA_WRITE_ENABLE;
    bool mask_red = control_0 & NV_PGRAPH_CONTROL_0_RED_WRITE_ENABLE;
    bool mask_green = control_0 & NV_PGRAPH_CONTROL_0_GREEN_WRITE_ENABLE;
    bool mask_blue = control_0 & NV_PGRAPH_CONTROL_0_BLUE_WRITE_ENABLE;
    bool color_write = mask_alpha || mask_red || mask_green || mask_blue;
    bool depth_test = control_0 & NV_PGRAPH_CONTROL_0_ZENABLE;
    bool stencil_test = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1) &
                        NV_PGRAPH_CONTROL_1_STENCIL_TEST_ENABLE;
    bool is_nop_draw = !(color_write || depth_test || stencil_test);

    pgraph_wgpu_surface_update(d, true, true, depth_test || stencil_test);

    if (is_nop_draw) {
        NV2A_DPRINTF("nop!\n");
        return;
    }
}

void pgraph_wgpu_draw_end(NV2AState *d)
{
    XSTAT_INC(n_draw);
    PGRAPHState *pg = &d->pgraph;
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;

    uint32_t control_0 = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0);
    bool mask_alpha = control_0 & NV_PGRAPH_CONTROL_0_ALPHA_WRITE_ENABLE;
    bool mask_red = control_0 & NV_PGRAPH_CONTROL_0_RED_WRITE_ENABLE;
    bool mask_green = control_0 & NV_PGRAPH_CONTROL_0_GREEN_WRITE_ENABLE;
    bool mask_blue = control_0 & NV_PGRAPH_CONTROL_0_BLUE_WRITE_ENABLE;
    bool color_write = mask_alpha || mask_red || mask_green || mask_blue;
    bool depth_test = control_0 & NV_PGRAPH_CONTROL_0_ZENABLE;
    bool stencil_test = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1) &
                        NV_PGRAPH_CONTROL_1_STENCIL_TEST_ENABLE;
    bool is_nop_draw = !(color_write || depth_test || stencil_test);

    if (is_nop_draw) {
        // FIXME: Check PGRAPH register 0x880.
        // HW uses bit 11 in 0x880 to enable or disable a color/zeta limit
        // check that will raise an exception in the case that a draw should
        // modify the color and/or zeta buffer but the target(s) are masked
        // off. This check only seems to trigger during the fragment
        // processing, it is legal to attempt a draw that is entirely
        // clipped regardless of 0x880. See xemu#635 for context.
        return;
    }

    pgraph_wgpu_flush_draw(d);

    pg->draw_time++;
    if (r->color_binding && pgraph_color_write_enabled(pg)) {
        r->color_binding->draw_time = pg->draw_time;
    }
    if (r->zeta_binding && pgraph_zeta_write_enabled(pg)) {
        r->zeta_binding->draw_time = pg->draw_time;
    }

    pgraph_wgpu_set_surface_dirty(pg, color_write, depth_test || stencil_test);
}

static int compare_memory_sync_requirement_by_addr(const void *p1,
                                                   const void *p2)
{
    const WgpuMemorySyncRequirement *l = p1, *r = p2;
    if (l->addr < r->addr)
        return -1;
    if (l->addr > r->addr)
        return 1;
    return 0;
}

static void sync_vertex_ram_buffer(PGRAPHState *pg)
{
    NV2AState *d = container_of(pg, NV2AState, pgraph);
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;
    PGRAPHWgpuDrawState *ds = &r->draw;

    if (ds->num_vertex_ram_buffer_syncs == 0) {
        return;
    }

    // Align sync requirements to page boundaries
    for (int i = 0; i < ds->num_vertex_ram_buffer_syncs; i++) {
        hwaddr start_addr =
            ds->vertex_ram_buffer_syncs[i].addr & TARGET_PAGE_MASK;
        hwaddr end_addr = ds->vertex_ram_buffer_syncs[i].addr +
                          ds->vertex_ram_buffer_syncs[i].size;
        end_addr = ROUND_UP(end_addr, TARGET_PAGE_SIZE);

        ds->vertex_ram_buffer_syncs[i].addr = start_addr;
        ds->vertex_ram_buffer_syncs[i].size = end_addr - start_addr;
    }

    // Sort the requirements in increasing order of addresses
    qsort(ds->vertex_ram_buffer_syncs, ds->num_vertex_ram_buffer_syncs,
          sizeof(WgpuMemorySyncRequirement),
          compare_memory_sync_requirement_by_addr);

    // Merge overlapping/adjacent requests to minimize number of tests
    WgpuMemorySyncRequirement merged[16];
    int num_syncs = 1;

    merged[0] = ds->vertex_ram_buffer_syncs[0];

    for (int i = 1; i < ds->num_vertex_ram_buffer_syncs; i++) {
        WgpuMemorySyncRequirement *p = &merged[num_syncs - 1];
        WgpuMemorySyncRequirement *t = &ds->vertex_ram_buffer_syncs[i];

        if (t->addr <= (p->addr + p->size)) {
            // Merge with previous
            hwaddr p_end_addr = p->addr + p->size;
            hwaddr t_end_addr = t->addr + t->size;
            hwaddr new_end_addr = MAX(p_end_addr, t_end_addr);
            p->size = new_end_addr - p->addr;
        } else {
            merged[num_syncs++] = *t;
        }
    }

    hwaddr vram_size = memory_region_size(d->vram);
    for (int i = 0; i < num_syncs; i++) {
        hwaddr addr = merged[i].addr;
        hwaddr size = merged[i].size;

        if (addr >= vram_size) {
            continue;
        }
        size = MIN(size, vram_size - addr);

        if (memory_region_test_and_clear_dirty(d->vram, addr, size,
                                               DIRTY_MEMORY_NV2A)) {
            pgraph_wgpu_update_vertex_ram_buffer(pg, addr, d->vram_ptr + addr,
                                                 size);
        }
    }

    ds->num_vertex_ram_buffer_syncs = 0;
}

void pgraph_wgpu_set_surface_dirty(PGRAPHState *pg, bool color, bool zeta)
{
    NV2A_DPRINTF("pgraph_set_surface_dirty(%d, %d) -- %d %d\n", color, zeta,
                 pgraph_color_write_enabled(pg), pgraph_zeta_write_enabled(pg));

    PGRAPHWgpuState *r = pg->wgpu_renderer_state;

    /* FIXME: Does this apply to CLEARs too? */
    color = color && pgraph_color_write_enabled(pg);
    zeta = zeta && pgraph_zeta_write_enabled(pg);
    pg->surface_color.draw_dirty |= color;
    pg->surface_zeta.draw_dirty |= zeta;

    NV2AState *d = container_of(pg, NV2AState, pgraph);
    if (r->color_binding && color) {
        /* GPU wrote it: trap the next CPU access (download first) */
        pgraph_wgpu_surface_rearm_cpu_trap(d, r->color_binding);
    }
    if (r->zeta_binding && zeta) {
        pgraph_wgpu_surface_rearm_cpu_trap(d, r->zeta_binding);
    }

    if (r->color_binding) {
        r->color_binding->draw_dirty |= color;
        r->color_binding->frame_time = pg->frame_time;
        r->color_binding->cleared = false;
    }

    if (r->zeta_binding) {
        r->zeta_binding->draw_dirty |= zeta;
        r->zeta_binding->frame_time = pg->frame_time;
        r->zeta_binding->cleared = false;
    }
}

void pgraph_wgpu_clear_surface(NV2AState *d, uint32_t parameter)
{
    XSTAT_INC(n_clear);
    PGRAPHState *pg = &d->pgraph;
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;
    PGRAPHWgpuDrawState *ds = &r->draw;

    nv2a_profile_inc_counter(NV2A_PROF_CLEAR);

    bool write_color = (parameter & NV097_CLEAR_SURFACE_COLOR);
    bool write_zeta =
        (parameter & (NV097_CLEAR_SURFACE_Z | NV097_CLEAR_SURFACE_STENCIL));

    pg->clearing = true;

    // FIXME: If doing a full surface clear, mark the surface for full clear
    // and we can just do the clear as part of the surface load.
    pgraph_wgpu_surface_update(d, true, write_color, write_zeta);

    SurfaceBinding *binding = r->color_binding ?: r->zeta_binding;
    if (!binding) {
        /* Nothing bound to clear */
        pg->clearing = false;
        return;
    }

    ds->clear_parameter = parameter;

    uint32_t clearrectx = pgraph_reg_r(pg, NV_PGRAPH_CLEARRECTX);
    uint32_t clearrecty = pgraph_reg_r(pg, NV_PGRAPH_CLEARRECTY);

    unsigned int xmin = GET_MASK(clearrectx, NV_PGRAPH_CLEARRECTX_XMIN);
    unsigned int xmax = GET_MASK(clearrectx, NV_PGRAPH_CLEARRECTX_XMAX);
    unsigned int ymin = GET_MASK(clearrecty, NV_PGRAPH_CLEARRECTY_YMIN);
    unsigned int ymax = GET_MASK(clearrecty, NV_PGRAPH_CLEARRECTY_YMAX);

    NV2A_DPRINTF("CLEAR min=(%d,%d) max=(%d,%d)%s%s\n", xmin, ymin, xmax, ymax,
                 write_color ? " color" : "", write_zeta ? " zeta" : "");

    // FIXME: What does hardware do when min >= max?
    // FIXME: What does hardware do when min >= surface size?
    xmin = MIN(xmin, binding->width - 1);
    ymin = MIN(ymin, binding->height - 1);
    xmax = MAX(xmin, MIN(xmax, binding->width - 1));
    ymax = MAX(ymin, MIN(ymax, binding->height - 1));

    unsigned int scissor_width = MAX(0, xmax - xmin + 1);
    unsigned int scissor_height = MAX(0, ymax - ymin + 1);

    pgraph_apply_anti_aliasing_factor(pg, &xmin, &ymin);
    pgraph_apply_anti_aliasing_factor(pg, &scissor_width, &scissor_height);

    pgraph_apply_scaling_factor(pg, &xmin, &ymin);
    pgraph_apply_scaling_factor(pg, &scissor_width, &scissor_height);

    float color[4] = { 0, 0, 0, 0 };
    float depth_value = 1.0f;
    int stencil_value = 0;
    if (write_color && r->color_binding) {
        pgraph_get_clear_color(pg, color);
    }
    if (write_zeta && r->zeta_binding) {
        pgraph_get_clear_depth_stencil_value(pg, &depth_value, &stencil_value);
    }

    bool do_color = write_color && r->color_binding;
    bool all_color_channels =
        (parameter & NV097_CLEAR_SURFACE_COLOR) ==
        (NV097_CLEAR_SURFACE_R | NV097_CLEAR_SURFACE_G | NV097_CLEAR_SURFACE_B |
         NV097_CLEAR_SURFACE_A);
    bool do_depth = r->zeta_binding && (parameter & NV097_CLEAR_SURFACE_Z) &&
                    r->zeta_binding->host_fmt.depth;
    bool do_stencil = r->zeta_binding &&
                      (parameter & NV097_CLEAR_SURFACE_STENCIL) &&
                      r->zeta_binding->host_fmt.stencil;

    if (!do_color && !do_depth && !do_stencil) {
        pg->clearing = false;
        pgraph_wgpu_set_surface_dirty(pg, write_color, write_zeta);
        return;
    }

    uint32_t att_width = wgpuTextureGetWidth(binding->texture);
    uint32_t att_height = wgpuTextureGetHeight(binding->texture);
    bool full_rect = xmin == 0 && ymin == 0 && scissor_width >= att_width &&
                     scissor_height >= att_height;

    if (full_rect && (!do_color || all_color_channels)) {
        /* Whole attachments / aspects: clear with the render pass loadOp */
        pgraph_wgpu_ensure_command_buffer(pg);
        pgraph_wgpu_end_render_pass(pg);
        if (ds->query_in_flight) {
            pgraph_wgpu_end_query(pg);
        }
        r->framebuffer_dirty = false;

        WGPUColor clear_color = { color[0], color[1], color[2], color[3] };
        uint32_t clear_stencil = stencil_value;
        pgraph_wgpu_begin_render_pass(pg, do_color ? &clear_color : NULL,
                                      do_depth ? &depth_value : NULL,
                                      do_stencil ? &clear_stencil : NULL);
    } else if (ensure_buffer_space(pg, WGPU_BUFFER_VERTEX_INLINE,
                                   12 * sizeof(float)) &&
               begin_pre_draw(pg, WGPUPrimitiveTopology_TriangleList)) {
        /* Partial / masked clear: full-screen triangle, scissored */
        float verts[12] = {
            -1.0f, -1.0f, depth_value, 1.0f,
             3.0f, -1.0f, depth_value, 1.0f,
            -1.0f,  3.0f, depth_value, 1.0f,
        };
        void *data = verts;
        size_t size = sizeof(verts);
        size_t offset =
            pgraph_wgpu_update_vertex_inline_buffer(pg, &data, &size, 1);

        begin_draw(pg);

        xmin = MIN(xmin, ds->pass_width);
        ymin = MIN(ymin, ds->pass_height);
        scissor_width = MIN(scissor_width, ds->pass_width - xmin);
        scissor_height = MIN(scissor_height, ds->pass_height - ymin);
        wgpuRenderPassEncoderSetScissorRect(ds->pass, xmin, ymin,
                                            scissor_width, scissor_height);
        /* this clear's scissor/blend/stencil: re-send the draw state next */
        memset(ds->pass_sc, 0xff, sizeof(ds->pass_sc));
        ds->pass_pipeline = NULL;
        ds->pass_vb_buf[0] = 0;     /* slot 0 = the clear triangle below */
        if (do_color) {
            WGPUColor blend_constant = { color[0], color[1], color[2],
                                         color[3] };
            wgpuRenderPassEncoderSetBlendConstant(ds->pass, &blend_constant);
        }
        if (do_stencil) {
            wgpuRenderPassEncoderSetStencilReference(ds->pass, stencil_value);
        }
        if (scissor_width && scissor_height) {
            wgpuRenderPassEncoderSetVertexBuffer(
                ds->pass, 0,
                ds->storage_buffers[WGPU_BUFFER_VERTEX_INLINE].buffer, offset,
                size);
            wgpuRenderPassEncoderDraw(ds->pass, 3, 1, 0, 0);
        }
        end_draw(pg);
    }

    pg->clearing = false;

    pgraph_wgpu_set_surface_dirty(pg, write_color, write_zeta);
}

/* Record the prepared draw: indices in ds->indices, vertex layout from
 * pgraph_wgpu_bind_vertex_attributes*() */
static void draw_indexed(NV2AState *d, const PrimConv *pc)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;
    PGRAPHWgpuDrawState *ds = &r->draw;

    if (ds->num_indices == 0) {
        return;
    }

    size_t index_size = ds->num_indices * sizeof(uint32_t);
    size_t vertex_size = pgraph_wgpu_vertex_upload_size(pg);

    if (!ensure_buffer_space(pg, WGPU_BUFFER_INDEX, index_size) ||
        !ensure_buffer_space(pg, WGPU_BUFFER_VERTEX_INLINE, vertex_size)) {
        return;
    }

    if (!begin_pre_draw(pg, pc->topology)) {
        return;
    }
    if (pc->out == PRIM_OUT_TRIANGLES && ds->pipeline_binding->cull_all) {
        return;
    }

    XPHASE_SET(XPHASE_GPU, "draw:vertex");
    pgraph_wgpu_upload_vertex_data(d);
    XPHASE_SET(XPHASE_GPU, "draw");
    size_t index_offset =
        pgraph_wgpu_update_index_buffer(pg, ds->indices, index_size);

    XPHASE_SET(XPHASE_GPU, "draw:encode");
    begin_draw(pg);
    if (pgraph_wgpu_set_vertex_buffers(pg)) {
        /*
         * The index buffer is append-only with a fixed size (running out of
         * space finishes the pass): bind it whole once per pass and select
         * this draw's slice with firstIndex.
         */
        if (!ds->pass_index_bound) {
            wgpuRenderPassEncoderSetIndexBuffer(
                ds->pass, ds->storage_buffers[WGPU_BUFFER_INDEX].buffer,
                WGPUIndexFormat_Uint32, 0, WGPU_WHOLE_SIZE);
            ds->pass_index_bound = true;
        }
        wgpuRenderPassEncoderDrawIndexed(ds->pass, ds->num_indices, 1,
                                         index_offset / sizeof(uint32_t), 0,
                                         0);
    }
    end_draw(pg);
}

static void flush_draw_impl(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;
    PGRAPHWgpuDrawState *ds = &r->draw;

    if (!(r->color_binding || r->zeta_binding)) {
        NV2A_DPRINTF("No binding present!!!\n");
        return;
    }

    ds->num_vertex_ram_buffer_syncs = 0;
    ds->num_indices = 0;

    PrimConv pc;
    get_prim_conv(pg, &pc);
    IndexGen g = { .ds = ds, .pc = &pc };

    if (pg->draw_arrays_length) {
        nv2a_profile_inc_counter(NV2A_PROF_DRAW_ARRAYS);

        assert(pg->inline_elements_length == 0);
        assert(pg->inline_buffer_length == 0);
        assert(pg->inline_array_length == 0);

        pgraph_wgpu_bind_vertex_attributes(d, pg->draw_arrays_min_start,
                                           pg->draw_arrays_max_count - 1, false,
                                           0, pg->draw_arrays_max_count - 1);
        for (int i = 0; i < pg->draw_arrays_length; i++) {
            g.elements = NULL;
            g.base = pg->draw_arrays_start[i] - pg->draw_arrays_min_start;
            gen_prim_indices(&g, pg->draw_arrays_count[i]);
        }
        sync_vertex_ram_buffer(pg);
        draw_indexed(d, &pc);
    } else if (pg->inline_elements_length) {
        assert(pg->inline_buffer_length == 0);
        assert(pg->inline_array_length == 0);

        nv2a_profile_inc_counter(NV2A_PROF_INLINE_ELEMENTS);

        uint32_t min_element = (uint32_t)-1;
        uint32_t max_element = 0;
        for (int i = 0; i < pg->inline_elements_length; i++) {
            max_element = MAX(pg->inline_elements[i], max_element);
            min_element = MIN(pg->inline_elements[i], min_element);
        }
        pgraph_wgpu_bind_vertex_attributes(
            d, min_element, max_element, false, 0,
            pg->inline_elements[pg->inline_elements_length - 1]);
        g.elements = pg->inline_elements;
        g.base = min_element;
        gen_prim_indices(&g, pg->inline_elements_length);
        sync_vertex_ram_buffer(pg);
        draw_indexed(d, &pc);
    } else if (pg->inline_buffer_length) {
        nv2a_profile_inc_counter(NV2A_PROF_INLINE_BUFFERS);
        assert(pg->inline_array_length == 0);

        pgraph_wgpu_bind_vertex_attributes_inline(d);
        for (int i = 0; i < NV2A_VERTEXSHADER_ATTRIBUTES; i++) {
            if (ds->repack.attrs & (1 << i)) {
                pg->vertex_attributes[i].inline_buffer_populated = false;
            }
        }
        g.elements = NULL;
        g.base = 0;
        gen_prim_indices(&g, pg->inline_buffer_length);
        draw_indexed(d, &pc);
    } else if (pg->inline_array_length) {
        nv2a_profile_inc_counter(NV2A_PROF_INLINE_ARRAYS);

        unsigned int offset = 0;
        for (int i = 0; i < NV2A_VERTEXSHADER_ATTRIBUTES; i++) {
            VertexAttribute *attr = &pg->vertex_attributes[i];
            if (attr->count == 0) {
                continue;
            }

            /* FIXME: Double check */
            offset = ROUND_UP(offset, attr->size);
            attr->inline_array_offset = offset;
            NV2A_DPRINTF("bind inline attribute %d size=%d, count=%d\n", i,
                         attr->size, attr->count);
            offset += attr->size * attr->count;
            offset = ROUND_UP(offset, attr->size);
        }

        unsigned int vertex_size = offset;
        if (!vertex_size) {
            return;
        }
        unsigned int index_count = pg->inline_array_length * 4 / vertex_size;
        if (!index_count) {
            return;
        }

        NV2A_DPRINTF("draw inline array %d, %d\n", vertex_size, index_count);
        pgraph_wgpu_bind_vertex_attributes(d, 0, index_count - 1, true,
                                           vertex_size, index_count - 1);
        g.elements = NULL;
        g.base = 0;
        gen_prim_indices(&g, index_count);
        draw_indexed(d, &pc);
    } else {
        NV2A_DPRINTF("EMPTY NV097_SET_BEGIN_END\n");
        NV2A_UNCONFIRMED("EMPTY NV097_SET_BEGIN_END");
    }
}

void pgraph_wgpu_flush_draw(NV2AState *d)
{
    XSTAT_T0();
    XPHASE_SET(XPHASE_GPU, "draw");
    flush_draw_impl(d);
    XPHASE_SET(XPHASE_GPU, NULL);
    XSTAT_T1(ns_draw);
}
