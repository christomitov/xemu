/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Geforce NV2A PGRAPH WebGPU Renderer: native linear BGRA byte reshaping
 * Render-pass scaffolding adapted from texture.c:
 * Copyright (c) 2012 espes
 * Copyright (c) 2015 Jannik Vogel
 * Copyright (c) 2018-2024 Matt Borgerson
 */

#include "renderer.h"

bool pgraph_wgpu_linear_reshape_enabled(void)
{
#ifdef EMSCRIPTEN
    static int enabled = -1;
    if (enabled < 0) {
        const char *e = getenv("XEMU_WASM_LINEAR_RESHAPE");
        enabled = !(e && *e == '0');
    }
    return enabled;
#else
    return false;
#endif
}

bool pgraph_wgpu_is_linear_bgra(const SurfaceBinding *s)
{
    return s->color && !s->swizzle && s->width && s->height &&
           s->host_fmt.format == WGPUTextureFormat_BGRA8Unorm &&
           s->host_fmt.conv == WGPU_SURFACE_CONV_NONE &&
           s->fmt.bytes_per_pixel == 4 &&
           s->host_fmt.host_bytes_per_pixel == 4 &&
           s->pitch == (uint64_t)s->width * 4 &&
           s->size == (uint64_t)s->pitch * s->height &&
           s->size <= UINT32_MAX;
}

/*
 * Coordinates name guest words, not image positions. textureLoad + an
 * unblended BGRA8 render preserves all four UNORM8 bytes. Discard preserves
 * destination bytes outside the source interval (including partial rows).
 * p: source base, destination base, destination pitch, opaque alpha;
 * all addresses/pitches are in 32-bit guest words relative to a common base.
 */
static const char reshape_wgsl[] =
    "@group(0) @binding(0) var src: texture_2d<f32>;\n"
    "@group(0) @binding(1) var<uniform> p: vec4u;\n"
    "@vertex fn vs(@builtin(vertex_index) i: u32) -> @builtin(position) vec4f {\n"
    "    var v = array(vec2f(-1.0, -1.0), vec2f(3.0, -1.0), vec2f(-1.0, 3.0));\n"
    "    return vec4f(v[i], 0.0, 1.0);\n"
    "}\n"
    "@fragment fn fs(@builtin(position) pos: vec4f) -> @location(0) vec4f {\n"
    "    let dim = textureDimensions(src);\n"
    "    let a = p.y + u32(pos.y) * p.z + u32(pos.x);\n"
    "    if (a < p.x) { discard; }\n"
    "    let i = a - p.x;\n"
    "    if (i >= dim.x * dim.y) { discard; }\n"
    "    let xy = vec2u(i % dim.x, i / dim.x);\n"
    "    let c = textureLoad(src, vec2i(xy), 0);\n"
    "    return vec4f(c.rgb, select(c.a, 1.0, p.w != 0u));\n"
    "}\n";

static void init_reshape(PGRAPHWgpuState *r)
{
    if (r->surf.reshape_pipeline) {
        return;
    }
    WGPUBindGroupLayoutEntry entries[2] = {
        { .binding = 0, .visibility = WGPUShaderStage_Fragment,
          .texture = { .sampleType = WGPUTextureSampleType_UnfilterableFloat,
                       .viewDimension = WGPUTextureViewDimension_2D } },
        { .binding = 1, .visibility = WGPUShaderStage_Fragment,
          .buffer = { .type = WGPUBufferBindingType_Uniform,
                      .minBindingSize = 16 } },
    };
    r->surf.reshape_bgl = wgpuDeviceCreateBindGroupLayout(
        r->device, &(WGPUBindGroupLayoutDescriptor){
                       .entryCount = 2, .entries = entries });
    WGPUPipelineLayout layout = wgpuDeviceCreatePipelineLayout(
        r->device, &(WGPUPipelineLayoutDescriptor){
                       .bindGroupLayoutCount = 1,
                       .bindGroupLayouts = &r->surf.reshape_bgl });
    WGPUShaderModule module = pgraph_wgpu_create_wgsl_module(
        r, "linear surface byte reshape", reshape_wgsl);
    WGPUColorTargetState target = {
        .format = WGPUTextureFormat_BGRA8Unorm,
        .writeMask = WGPUColorWriteMask_All,
    };
    WGPUFragmentState fragment = {
        .module = module, .entryPoint = { "fs", WGPU_STRLEN },
        .targetCount = 1, .targets = &target,
    };
    r->surf.reshape_pipeline = wgpuDeviceCreateRenderPipeline(
        r->device, &(WGPURenderPipelineDescriptor){
                       .label = { "linear surface byte reshape", WGPU_STRLEN },
                       .layout = layout,
                       .vertex = { .module = module,
                                   .entryPoint = { "vs", WGPU_STRLEN } },
                       .primitive = {
                           .topology = WGPUPrimitiveTopology_TriangleList },
                       .multisample = { .count = 1, .mask = ~0u },
                       .fragment = &fragment });
    wgpuShaderModuleRelease(module);
    wgpuPipelineLayoutRelease(layout);
}

/* Caller guarantees a distinct, tightly packed BGRA8 destination image. */
void pgraph_wgpu_reshape_surface(PGRAPHState *pg, const SurfaceBinding *src,
                                 WGPUTextureView dst, hwaddr dst_addr,
                                 unsigned int width, unsigned int height,
                                 bool opaque)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;
    assert(pg->surface_scale_factor == 1 && pgraph_wgpu_is_linear_bgra(src));
    assert(width && height && src->view != dst);
    hwaddr base = MIN(src->vram_addr, dst_addr);
    assert((src->vram_addr - base) % 4 == 0 && (dst_addr - base) % 4 == 0);
    uint64_t src_start = (src->vram_addr - base) / 4;
    uint64_t dst_start = (dst_addr - base) / 4;
    uint64_t src_end = src_start + src->size / 4;
    uint64_t dst_end = dst_start + (uint64_t)width * height;
    assert(src_end <= UINT32_MAX && dst_end <= UINT32_MAX);
    uint64_t first = MAX(src_start, dst_start);
    uint64_t end = MIN(src_end, dst_end);
    assert(first < end);
    unsigned int first_row = (first - dst_start) / width;
    unsigned int last_row = (end - 1 - dst_start) / width;

    init_reshape(r);
    /*
     * A fresh immutable buffer per copy: queue writes may precede submission
     * of several reshapes in one encoder, but never overwrite earlier params.
     * Recorded commands retain buffers/bind groups after release, not destroy.
     */
    uint32_t params[4] = {
        (uint32_t)src_start, (uint32_t)dst_start, width, opaque,
    };
    WGPUBuffer uniform = wgpuDeviceCreateBuffer(
        r->device, &(WGPUBufferDescriptor){
                       .usage = WGPUBufferUsage_Uniform |
                                WGPUBufferUsage_CopyDst,
                       .size = sizeof(params) });
    wgpuQueueWriteBuffer(r->queue, uniform, 0, params, sizeof(params));
    WGPUBindGroupEntry entries[2] = {
        { .binding = 0, .textureView = src->view },
        { .binding = 1, .buffer = uniform, .size = sizeof(params) },
    };
    WGPUBindGroup group = wgpuDeviceCreateBindGroup(
        r->device, &(WGPUBindGroupDescriptor){
                       .layout = r->surf.reshape_bgl,
                       .entryCount = 2, .entries = entries });
    WGPUCommandEncoder enc = pgraph_wgpu_begin_nondraw_commands(pg);
    WGPURenderPassColorAttachment ca = {
        .view = dst, .depthSlice = WGPU_DEPTH_SLICE_UNDEFINED,
        .loadOp = WGPULoadOp_Load, .storeOp = WGPUStoreOp_Store,
    };
    WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(
        enc, &(WGPURenderPassDescriptor){
                 .label = { "linear surface byte reshape", WGPU_STRLEN },
                 .colorAttachmentCount = 1, .colorAttachments = &ca });
    wgpuRenderPassEncoderSetPipeline(pass, r->surf.reshape_pipeline);
    wgpuRenderPassEncoderSetBindGroup(pass, 0, group, 0, NULL);
    wgpuRenderPassEncoderSetScissorRect(pass, 0, first_row, width,
                                       last_row - first_row + 1);
    wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
    wgpuRenderPassEncoderEnd(pass);
    wgpuRenderPassEncoderRelease(pass);
    wgpuBindGroupRelease(group);
    wgpuBufferRelease(uniform);
    pgraph_wgpu_end_nondraw_commands(pg, enc);
}

void pgraph_wgpu_finalize_surface_reshape(PGRAPHState *pg)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;
    if (r->surf.reshape_pipeline) {
        wgpuRenderPipelineRelease(r->surf.reshape_pipeline);
        r->surf.reshape_pipeline = NULL;
    }
    if (r->surf.reshape_bgl) {
        wgpuBindGroupLayoutRelease(r->surf.reshape_bgl);
        r->surf.reshape_bgl = NULL;
    }
}
