/*
 * Geforce NV2A PGRAPH WebGPU Renderer: draw module (owned by the draw module;
 * included from renderer.h, do not include directly)
 *
 * Port of vk/draw.c, vk/vertex.c, vk/buffer.c, vk/command.c, vk/reports.c.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#ifndef HW_XBOX_NV2A_PGRAPH_WGPU_DRAW_H
#define HW_XBOX_NV2A_PGRAPH_WGPU_DRAW_H

/* ---- buffers (buffer.c) ---- */

enum WgpuBufferIndex {
    WGPU_BUFFER_INDEX,          /* Index|CopyDst, append-only per submit */
    WGPU_BUFFER_VERTEX_RAM,     /* Vertex|CopyDst, mirror of guest VRAM */
    WGPU_BUFFER_VERTEX_INLINE,  /* Vertex|CopyDst, append-only per submit */
    WGPU_BUFFER_QUERY_RESOLVE,  /* QueryResolve|CopySrc */
    WGPU_BUFFER_QUERY_READBACK, /* MapRead|CopyDst */
    WGPU_BUFFER_COUNT
};

typedef struct WgpuStorageBuffer {
    WGPUBuffer buffer;
    WGPUBufferUsage usage;
    const char *label;
    size_t buffer_offset; /* append offset, reset by pgraph_wgpu_finish */
    size_t buffer_size;
} WgpuStorageBuffer;

typedef struct WgpuMemorySyncRequirement {
    hwaddr addr, size;
} WgpuMemorySyncRequirement;

/* ---- pipelines (draw.c) ---- */

typedef struct WgpuVertexAttrKey {
    uint32_t format;   /* WGPUVertexFormat */
    uint32_t offset;   /* offset within the buffer's stride */
    uint8_t location;  /* NV2A attribute index == shader location */
    uint8_t buffer;    /* vertex buffer slot */
    uint8_t pad[2];
} WgpuVertexAttrKey;

typedef struct WgpuVertexLayoutKey {
    uint32_t num_buffers;
    uint32_t num_attrs;
    uint32_t strides[NV2A_VERTEXSHADER_ATTRIBUTES];
    WgpuVertexAttrKey attrs[NV2A_VERTEXSHADER_ATTRIBUTES];
} WgpuVertexLayoutKey;

typedef struct WgpuPipelineKey {
    uint32_t clear;
    uint32_t color_format; /* WGPUTextureFormat or Undefined */
    uint32_t zeta_format;
    uint32_t topology;     /* WGPUPrimitiveTopology */
    /* group 0 layout: ShaderBinding.pipeline_layout can change while the
     * ShaderBinding (and ShaderState) stay the same (shaders.h) */
    uint8_t layout_key[16];     /* ShaderLayoutKey bytes */
    uint64_t pipeline_layout;   /* WGPUPipelineLayout pointer */
    uint32_t regs[9];
    WgpuVertexLayoutKey vertex;
    ShaderState shader_state;
} WgpuPipelineKey;

typedef struct WgpuPipelineBinding WgpuPipelineBinding;
struct WgpuPipelineBinding {
    LruNode node;
    WgpuPipelineKey key;
    WGPURenderPipeline pipeline;
    unsigned int draw_time;
    WGPUColor blend_constant;
    uint32_t stencil_ref;
    bool cull_all; /* NV2A cull FRONT_AND_BACK: WebGPU has no equivalent */
};

/* ---- vertex input (vertex.c) ---- */

typedef enum WgpuVertexSource {
    WGPU_VSRC_VRAM,         /* WGPU_BUFFER_VERTEX_RAM, base = VRAM address */
    WGPU_VSRC_INLINE_ARRAY, /* inline array uploaded to VERTEX_INLINE */
    WGPU_VSRC_REPACK,       /* CPU-converted data uploaded to VERTEX_INLINE */
} WgpuVertexSource;

typedef struct WgpuVertexBufferBinding {
    WgpuVertexSource source;
    uint64_t base; /* byte offset of element min_element, relative to the
                    * source (VRAM address / start of uploaded data) */
} WgpuVertexBufferBinding;

/* Attributes that must be converted on the CPU (unsupported WebGPU format,
 * misaligned offset/stride, or out of vertex buffer slots). All of them are
 * interleaved into a single vertex buffer slot. */
typedef struct WgpuVertexRepack {
    uint16_t attrs;
    bool inline_buffer;    /* interleave attr->inline_buffer (float4 each) */
    int slot;              /* -1 if unused */
    uint32_t stride;       /* output bytes per vertex */
    uint32_t out_offset[NV2A_VERTEXSHADER_ATTRIBUTES];
    const uint8_t *src[NV2A_VERTEXSHADER_ATTRIBUTES]; /* element 0 */
    uint32_t src_stride[NV2A_VERTEXSHADER_ATTRIBUTES];
    const uint8_t *src_limit; /* end of readable source memory */
} WgpuVertexRepack;

/* ---- reports (reports.c) ---- */

typedef struct WgpuQueryReport {
    QSIMPLEQ_ENTRY(WgpuQueryReport) entry;
    bool clear;
    uint32_t parameter;
    unsigned int query_count;
} WgpuQueryReport;

#define WGPU_MAX_QUERIES_IN_FLIGHT 1024 /* logical (NV2A) queries per submit */
#define WGPU_MAX_GPU_QUERIES 4096       /* WebGPU occlusion queries/submit */

typedef struct PGRAPHWgpuDrawState {
    /* command state (command.c) */
    WGPUCommandEncoder encoder;
    bool in_command_buffer;
    unsigned int command_buffer_start_time;
    uint32_t submit_count;
    WGPURenderPassEncoder pass;
    bool in_render_pass;
    bool in_draw;
    WGPUTextureView pass_color_view, pass_zeta_view;
    /* depth target for passes with no zeta surface bound */
    WGPUTexture scratch_zeta;
    WGPUTextureView scratch_zeta_view;
    uint32_t scratch_zeta_w, scratch_zeta_h;
    uint32_t pass_width, pass_height; /* attachment size */
    WgpuPipelineBinding *pass_pipeline; /* bound in current pass */

    /* pipelines (draw.c) */
    Lru pipeline_cache;
    WgpuPipelineBinding *pipeline_cache_entries;
    WgpuPipelineBinding *pipeline_binding;
    bool pipeline_binding_changed;
    WGPUShaderModule clear_module;
    WGPUPipelineLayout empty_pipeline_layout;
    WGPUBindGroup bind_group; /* from pgraph_wgpu_update_bind_group */
    uint32_t clear_parameter;

    /* buffers (buffer.c) */
    WgpuStorageBuffer storage_buffers[WGPU_BUFFER_COUNT];
    WgpuMemorySyncRequirement
        vertex_ram_buffer_syncs[NV2A_VERTEXSHADER_ATTRIBUTES];
    size_t num_vertex_ram_buffer_syncs;
    unsigned long *uploaded_bitmap;
    size_t bitmap_size;

    /* vertex input (vertex.c) */
    WgpuVertexLayoutKey vertex_layout;
    WgpuVertexBufferBinding vertex_buffers[NV2A_VERTEXSHADER_ATTRIBUTES];
    WgpuVertexRepack repack;
    unsigned int min_element, num_elements;
    bool inline_array_mode;       /* upload pg->inline_array this draw */
    uint64_t inline_array_offset; /* in VERTEX_INLINE, set at upload */
    uint64_t repack_offset;       /* in VERTEX_INLINE, set at upload */
    uint8_t *scratch;             /* CPU staging for repack/interleave */
    size_t scratch_size;

    /* generated index list (draw.c) */
    uint32_t *indices;
    size_t num_indices, indices_capacity;

    /* reports (reports.c) */
    WGPUQuerySet query_set;
    int num_queries_in_flight; /* logical queries */
    bool new_query_needed;
    bool query_in_flight;      /* logical query active */
    int num_gpu_queries;       /* WebGPU occlusion queries this submit */
    bool gpu_query_active;     /* occlusion query open in current pass */
    int num_gpu_queries_resolved;  /* resolved in last submit, unread */
    uint16_t gpu_query_logical[WGPU_MAX_GPU_QUERIES];
    uint32_t zpass_pixel_count_result;
    QSIMPLEQ_HEAD(, WgpuQueryReport) report_queue;
} PGRAPHWgpuDrawState;

/* ---- draw module internal (and available to other modules) ---- */

/* buffer.c */
bool pgraph_wgpu_buffer_has_space_for(PGRAPHState *pg, int index, size_t size,
                                      size_t alignment);
/* Queue-write data[0..count) into buffer `index` at the current append
 * offset (each chunk aligned to `alignment`, which must be a multiple of 4;
 * sizes are padded to 4). Returns the offset of the first chunk. Space must
 * have been checked with pgraph_wgpu_buffer_has_space_for(). */
size_t pgraph_wgpu_append_to_buffer(PGRAPHState *pg, int index, void **data,
                                    size_t *sizes, size_t count,
                                    size_t alignment);

/* command.c */
void pgraph_wgpu_ensure_command_buffer(PGRAPHState *pg);
void pgraph_wgpu_begin_render_pass(PGRAPHState *pg,
                                   const WGPUColor *color_clear,
                                   const float *depth_clear,
                                   const uint32_t *stencil_clear);
void pgraph_wgpu_end_render_pass(PGRAPHState *pg);

/* vertex.c */
void pgraph_wgpu_bind_vertex_attributes(NV2AState *d, unsigned int min_element,
                                        unsigned int max_element,
                                        bool inline_data,
                                        unsigned int inline_stride,
                                        unsigned int provoking_element);
void pgraph_wgpu_bind_vertex_attributes_inline(NV2AState *d);
size_t pgraph_wgpu_vertex_upload_size(PGRAPHState *pg);
void pgraph_wgpu_upload_vertex_data(NV2AState *d);
bool pgraph_wgpu_set_vertex_buffers(PGRAPHState *pg);
size_t pgraph_wgpu_update_index_buffer(PGRAPHState *pg, void *data,
                                       size_t size);
size_t pgraph_wgpu_update_vertex_inline_buffer(PGRAPHState *pg, void **data,
                                               size_t *sizes, size_t count);

/* reports.c */
void pgraph_wgpu_begin_query(PGRAPHState *pg);
void pgraph_wgpu_end_query(PGRAPHState *pg);
void pgraph_wgpu_begin_gpu_query_if_needed(PGRAPHState *pg);
void pgraph_wgpu_end_gpu_query(PGRAPHState *pg);
void pgraph_wgpu_resolve_queries(PGRAPHState *pg);

WGPUTextureView pgraph_wgpu_scratch_zeta_view(PGRAPHWgpuState *r,
                                              uint32_t width, uint32_t height);

#endif
