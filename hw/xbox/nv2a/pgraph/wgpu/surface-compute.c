/*
 * Geforce NV2A PGRAPH WebGPU Renderer: depth/stencil surface pack/unpack
 *
 * Port of the Vulkan renderer's surface-compute.c (../vk/surface-compute.c).
 *
 * NV2A Z24S8 surfaces store depth in bits 31-8 and stencil in bits 7-0 of
 * each 32-bit word. The host keeps them in a combined depth-stencil texture
 * (Depth32FloatStencil8, or Depth24PlusStencil8 as fallback):
 *
 *  - pack (download): a compute shader textureLoad()s the DepthOnly and
 *    StencilOnly views and writes packed Z24S8 words into a storage buffer.
 *    (WebGPU cannot copy out Depth24Plus, so this path is used for both
 *    formats.)
 *  - unpack (upload): stencil bytes are extracted on the CPU and written
 *    with wgpuQueueWriteTexture (stencil8 aspects are copy destinations);
 *    depth is written by a full-screen render pass whose fragment shader
 *    outputs frag_depth from the guest words (depth aspects of
 *    Depth32Float/Depth24Plus are not copy destinations in WebGPU).
 *
 * The depth conversions match vk (see ../vk/surface-compute.c).
 *
 * Copyright (c) 2024 Matt Borgerson
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

#include "hw/xbox/nv2a/pgraph/pgraph.h"
#include "renderer.h"

// TODO: Swizzle/Unswizzle
// TODO: Float depth format (low priority, but would be better for accuracy)

static const char pack_wgsl[] =
    "@group(0) @binding(0) var depth_in: texture_depth_2d;\n"
    "@group(0) @binding(1) var stencil_in: texture_2d<u32>;\n"
    "@group(0) @binding(2) var<storage, read_write> depth_stencil_out: "
    "array<u32>;\n"
    "fn load_stencil(c: vec2i) -> u32 {\n"
    "    return textureLoad(stencil_in, c, 0).r & 0xffu;\n"
    "}\n"
    // D24 unorm: raw 24-bit value
    "@compute @workgroup_size(8, 8)\n"
    "fn pack_d24(@builtin(global_invocation_id) id: vec3u) {\n"
    "    let dim = textureDimensions(depth_in);\n"
    "    if (id.x >= dim.x || id.y >= dim.y) { return; }\n"
    "    let c = vec2i(id.xy);\n"
    "    let d = clamp(textureLoad(depth_in, c, 0), 0.0, 1.0);\n"
    "    let depth_value = u32(round(d * 16777215.0));\n"
    "    depth_stencil_out[id.y * dim.x + id.x] =\n"
    "        (depth_value << 8u) | load_stencil(c);\n"
    "}\n"
    // D32 float: same as vk pack_d32_sfloat_s8_uint_to_z24s8
    "@compute @workgroup_size(8, 8)\n"
    "fn pack_d32(@builtin(global_invocation_id) id: vec3u) {\n"
    "    let dim = textureDimensions(depth_in);\n"
    "    if (id.x >= dim.x || id.y >= dim.y) { return; }\n"
    "    let c = vec2i(id.xy);\n"
    "    let depth_value = u32(i32(textureLoad(depth_in, c, 0) * "
    "16777215.0));\n"
    "    depth_stencil_out[id.y * dim.x + id.x] =\n"
    "        (depth_value << 8u) | load_stencil(c);\n"
    "}\n";

static const char unpack_wgsl[] =
    "struct DepthStencilIn {\n"
    "    width: u32, height: u32, pad0: u32, pad1: u32,\n"
    "    data: array<u32>,\n"
    "}\n"
    "@group(0) @binding(0) var<storage, read> depth_stencil_in: "
    "DepthStencilIn;\n"
    "@vertex fn vs(@builtin(vertex_index) i: u32) -> @builtin(position) vec4f "
    "{\n"
    "    var p = array(vec2f(-1.0, -1.0), vec2f(3.0, -1.0), "
    "vec2f(-1.0, 3.0));\n"
    "    return vec4f(p[i], 0.0, 1.0);\n"
    "}\n"
    "struct FragOut { @builtin(frag_depth) depth: f32 };\n"
    "fn load_z24(pos: vec4f) -> u32 {\n"
    "    let x = u32(pos.x);\n"
    "    let y = u32(pos.y);\n"
    "    return depth_stencil_in.data[y * depth_stencil_in.width + x] >> 8u;\n"
    "}\n"
    "@fragment fn unpack_d24(@builtin(position) pos: vec4f) -> FragOut {\n"
    "    var o: FragOut;\n"
    "    o.depth = f32(load_z24(pos)) / 16777215.0;\n"
    "    return o;\n"
    "}\n"
    // Conversion to float depth must be the same as in fragment shader
    "@fragment fn unpack_d32(@builtin(position) pos: vec4f) -> FragOut {\n"
    "    var o: FragOut;\n"
    "    o.depth = bitcast<f32>(bitcast<u32>(f32(load_z24(pos)) / "
    "16777216.0) + 1u);\n"
    "    return o;\n"
    "}\n";

/*
 * GPU-sourced unpack: stencil bytes of the packed words (header + data, as
 * for unpack) into rows of @stride_words u32s, 4 bytes per word, for a
 * buffer-to-texture copy into the stencil aspect.
 */
static const char stencil_extract_wgsl[] =
    "struct DepthStencilIn {\n"
    "    width: u32, height: u32, stride_words: u32, pad1: u32,\n"
    "    data: array<u32>,\n"
    "}\n"
    "@group(0) @binding(0) var<storage, read> inp: DepthStencilIn;\n"
    "@group(0) @binding(1) var<storage, read_write> outp: array<u32>;\n"
    "@compute @workgroup_size(64)\n"
    "fn extract(@builtin(global_invocation_id) id: vec3u) {\n"
    "    let w = inp.width;\n"
    "    if (id.x >= (w + 3u) / 4u || id.y >= inp.height) { return; }\n"
    "    var v = 0u;\n"
    "    for (var k = 0u; k < 4u; k++) {\n"
    "        let x = id.x * 4u + k;\n"
    "        if (x < w) {\n"
    "            v |= (inp.data[id.y * w + x] & 0xffu) << (8u * k);\n"
    "        }\n"
    "    }\n"
    "    outp[id.y * inp.stride_words + id.x] = v;\n"
    "}\n";

#define UNPACK_HEADER_SIZE 16

static WGPUTextureFormat z24s8_host_format(PGRAPHWgpuState *r)
{
    return r->surf.kelvin_surface_zeta_map[NV097_SET_SURFACE_FORMAT_ZETA_Z24S8]
        .format;
}

static bool z24s8_host_is_float(PGRAPHWgpuState *r)
{
    return z24s8_host_format(r) == WGPUTextureFormat_Depth32FloatStencil8;
}

static void create_pack_pipeline(PGRAPHWgpuState *r)
{
    WGPUBindGroupLayoutEntry entries[3] = {
        { .binding = 0, .visibility = WGPUShaderStage_Compute,
          .texture = { .sampleType = WGPUTextureSampleType_Depth,
                       .viewDimension = WGPUTextureViewDimension_2D } },
        { .binding = 1, .visibility = WGPUShaderStage_Compute,
          .texture = { .sampleType = WGPUTextureSampleType_Uint,
                       .viewDimension = WGPUTextureViewDimension_2D } },
        { .binding = 2, .visibility = WGPUShaderStage_Compute,
          .buffer = { .type = WGPUBufferBindingType_Storage } },
    };
    r->surf.compute.pack_bgl = wgpuDeviceCreateBindGroupLayout(
        r->device, &(WGPUBindGroupLayoutDescriptor){
                       .label = { "zeta pack", WGPU_STRLEN },
                       .entryCount = ARRAY_SIZE(entries),
                       .entries = entries });
    WGPUPipelineLayout layout = wgpuDeviceCreatePipelineLayout(
        r->device, &(WGPUPipelineLayoutDescriptor){
                       .bindGroupLayoutCount = 1,
                       .bindGroupLayouts = &r->surf.compute.pack_bgl });

    WGPUShaderModule module =
        pgraph_wgpu_create_wgsl_module(r, "zeta pack", pack_wgsl);

    WGPUComputePipelineDescriptor desc = WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
    desc.label = (WGPUStringView){ "zeta pack", WGPU_STRLEN };
    desc.layout = layout;
    desc.compute.module = module;
    desc.compute.entryPoint = (WGPUStringView){
        z24s8_host_is_float(r) ? "pack_d32" : "pack_d24", WGPU_STRLEN
    };
    r->surf.compute.pack_pipeline =
        wgpuDeviceCreateComputePipeline(r->device, &desc);

    wgpuShaderModuleRelease(module);
    wgpuPipelineLayoutRelease(layout);
}

static void create_unpack_pipeline(PGRAPHWgpuState *r)
{
    WGPUBindGroupLayoutEntry entry = {
        .binding = 0,
        .visibility = WGPUShaderStage_Fragment,
        .buffer = { .type = WGPUBufferBindingType_ReadOnlyStorage },
    };
    r->surf.compute.unpack_bgl = wgpuDeviceCreateBindGroupLayout(
        r->device, &(WGPUBindGroupLayoutDescriptor){
                       .label = { "zeta unpack", WGPU_STRLEN },
                       .entryCount = 1,
                       .entries = &entry });
    WGPUPipelineLayout layout = wgpuDeviceCreatePipelineLayout(
        r->device, &(WGPUPipelineLayoutDescriptor){
                       .bindGroupLayoutCount = 1,
                       .bindGroupLayouts = &r->surf.compute.unpack_bgl });

    WGPUShaderModule module =
        pgraph_wgpu_create_wgsl_module(r, "zeta unpack", unpack_wgsl);

    WGPUDepthStencilState ds = WGPU_DEPTH_STENCIL_STATE_INIT;
    ds.format = z24s8_host_format(r);
    ds.depthWriteEnabled = WGPUOptionalBool_True;
    ds.depthCompare = WGPUCompareFunction_Always;
    ds.stencilFront = (WGPUStencilFaceState){
        .compare = WGPUCompareFunction_Always,
        .failOp = WGPUStencilOperation_Keep,
        .depthFailOp = WGPUStencilOperation_Keep,
        .passOp = WGPUStencilOperation_Keep,
    };
    ds.stencilBack = ds.stencilFront;
    ds.stencilReadMask = 0xff;
    ds.stencilWriteMask = 0; /* stencil is uploaded by a queue write */

    WGPUFragmentState frag = {
        .module = module,
        .entryPoint = { z24s8_host_is_float(r) ? "unpack_d32" : "unpack_d24",
                        WGPU_STRLEN },
        .targetCount = 0,
    };
    WGPURenderPipelineDescriptor desc = {
        .label = { "zeta unpack", WGPU_STRLEN },
        .layout = layout,
        .vertex = { .module = module, .entryPoint = { "vs", WGPU_STRLEN } },
        .primitive = { .topology = WGPUPrimitiveTopology_TriangleList },
        .depthStencil = &ds,
        .multisample = { .count = 1, .mask = ~0u },
        .fragment = &frag,
    };
    r->surf.compute.unpack_pipeline =
        wgpuDeviceCreateRenderPipeline(r->device, &desc);

    wgpuShaderModuleRelease(module);
    wgpuPipelineLayoutRelease(layout);
}

static void create_stencil_extract_pipeline(PGRAPHWgpuState *r)
{
    WGPUBindGroupLayoutEntry entries[2] = {
        { .binding = 0, .visibility = WGPUShaderStage_Compute,
          .buffer = { .type = WGPUBufferBindingType_ReadOnlyStorage } },
        { .binding = 1, .visibility = WGPUShaderStage_Compute,
          .buffer = { .type = WGPUBufferBindingType_Storage } },
    };
    r->surf.compute.stencil_bgl = wgpuDeviceCreateBindGroupLayout(
        r->device, &(WGPUBindGroupLayoutDescriptor){
                       .label = { "zeta stencil extract", WGPU_STRLEN },
                       .entryCount = ARRAY_SIZE(entries),
                       .entries = entries });
    WGPUPipelineLayout layout = wgpuDeviceCreatePipelineLayout(
        r->device, &(WGPUPipelineLayoutDescriptor){
                       .bindGroupLayoutCount = 1,
                       .bindGroupLayouts = &r->surf.compute.stencil_bgl });
    WGPUShaderModule module = pgraph_wgpu_create_wgsl_module(
        r, "zeta stencil extract", stencil_extract_wgsl);

    WGPUComputePipelineDescriptor desc = WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
    desc.label = (WGPUStringView){ "zeta stencil extract", WGPU_STRLEN };
    desc.layout = layout;
    desc.compute.module = module;
    desc.compute.entryPoint = (WGPUStringView){ "extract", WGPU_STRLEN };
    r->surf.compute.stencil_pipeline =
        wgpuDeviceCreateComputePipeline(r->device, &desc);

    wgpuShaderModuleRelease(module);
    wgpuPipelineLayoutRelease(layout);
}

static void ensure_buffer(PGRAPHWgpuState *r, WGPUBuffer *buffer,
                          size_t *buffer_size, size_t size,
                          WGPUBufferUsage usage, const char *label)
{
    if (*buffer && *buffer_size >= size) {
        return;
    }
    if (*buffer) {
        /* Release only: already-recorded commands keep the old one alive */
        wgpuBufferRelease(*buffer);
    }
    size = ROUND_UP(size, 64 * 1024);
    *buffer = wgpuDeviceCreateBuffer(
        r->device, &(WGPUBufferDescriptor){ .label = { label, WGPU_STRLEN },
                                            .usage = usage,
                                            .size = size });
    *buffer_size = size;
}

void pgraph_wgpu_init_surface_compute(PGRAPHState *pg)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;

    create_pack_pipeline(r);
    create_unpack_pipeline(r);
    create_stencil_extract_pipeline(r);
}

void pgraph_wgpu_finalize_surface_compute(PGRAPHState *pg)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;

    if (r->surf.compute.pack_pipeline) {
        wgpuComputePipelineRelease(r->surf.compute.pack_pipeline);
        r->surf.compute.pack_pipeline = NULL;
    }
    if (r->surf.compute.pack_bgl) {
        wgpuBindGroupLayoutRelease(r->surf.compute.pack_bgl);
        r->surf.compute.pack_bgl = NULL;
    }
    if (r->surf.compute.unpack_pipeline) {
        wgpuRenderPipelineRelease(r->surf.compute.unpack_pipeline);
        r->surf.compute.unpack_pipeline = NULL;
    }
    if (r->surf.compute.unpack_bgl) {
        wgpuBindGroupLayoutRelease(r->surf.compute.unpack_bgl);
        r->surf.compute.unpack_bgl = NULL;
    }
    if (r->surf.compute.pack_dst) {
        wgpuBufferRelease(r->surf.compute.pack_dst);
        r->surf.compute.pack_dst = NULL;
        r->surf.compute.pack_dst_size = 0;
    }
    if (r->surf.compute.unpack_src) {
        wgpuBufferRelease(r->surf.compute.unpack_src);
        r->surf.compute.unpack_src = NULL;
        r->surf.compute.unpack_src_size = 0;
    }
    if (r->surf.compute.stencil_pipeline) {
        wgpuComputePipelineRelease(r->surf.compute.stencil_pipeline);
        r->surf.compute.stencil_pipeline = NULL;
    }
    if (r->surf.compute.stencil_bgl) {
        wgpuBindGroupLayoutRelease(r->surf.compute.stencil_bgl);
        r->surf.compute.stencil_bgl = NULL;
    }
    if (r->surf.compute.xfer) {
        wgpuBufferRelease(r->surf.compute.xfer);
        r->surf.compute.xfer = NULL;
        r->surf.compute.xfer_size = 0;
    }
    if (r->surf.compute.stencil_dst) {
        wgpuBufferRelease(r->surf.compute.stencil_dst);
        r->surf.compute.stencil_dst = NULL;
        r->surf.compute.stencil_dst_size = 0;
    }
}

//
// Pack depth+stencil into NV097_SET_SURFACE_FORMAT_ZETA_Z24S8
// formatted buffer with depth in bits 31-8 and stencil in bits 7-0.
//
void pgraph_wgpu_pack_depth_stencil(PGRAPHState *pg, SurfaceBinding *surface,
                                    WGPUCommandEncoder enc)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;

    assert(surface->host_fmt.conv == WGPU_SURFACE_CONV_Z24S8);
    assert(surface->depth_view && surface->stencil_view);

    unsigned int width = surface->width ? surface->width : 1;
    unsigned int height = surface->height ? surface->height : 1;
    pgraph_apply_scaling_factor(pg, &width, &height);

    size_t output_size = (size_t)width * height * 4;
    ensure_buffer(r, &r->surf.compute.pack_dst, &r->surf.compute.pack_dst_size,
                  output_size,
                  WGPUBufferUsage_Storage | WGPUBufferUsage_CopySrc,
                  "zeta pack dst");

    WGPUBindGroupEntry entries[3] = {
        { .binding = 0, .textureView = surface->depth_view },
        { .binding = 1, .textureView = surface->stencil_view },
        { .binding = 2, .buffer = r->surf.compute.pack_dst, .offset = 0,
          .size = output_size },
    };
    WGPUBindGroup bg = wgpuDeviceCreateBindGroup(
        r->device, &(WGPUBindGroupDescriptor){
                       .layout = r->surf.compute.pack_bgl,
                       .entryCount = ARRAY_SIZE(entries),
                       .entries = entries });

    WGPUComputePassEncoder pass = wgpuCommandEncoderBeginComputePass(
        enc, &(WGPUComputePassDescriptor){
                 .label = { "zeta pack", WGPU_STRLEN } });
    wgpuComputePassEncoderSetPipeline(pass, r->surf.compute.pack_pipeline);
    wgpuComputePassEncoderSetBindGroup(pass, 0, bg, 0, NULL);
    wgpuComputePassEncoderDispatchWorkgroups(pass, DIV_ROUND_UP(width, 8),
                                             DIV_ROUND_UP(height, 8), 1);
    wgpuComputePassEncoderEnd(pass);
    wgpuComputePassEncoderRelease(pass);
    wgpuBindGroupRelease(bg);
}

/* Depth from the header + Z24S8 words in unpack_src: full-screen pass. */
static void unpack_depth_pass(PGRAPHWgpuState *r, SurfaceBinding *surface,
                              WGPUCommandEncoder enc, size_t data_size)
{
    unsigned int width = surface->width, height = surface->height;

    WGPUBindGroupEntry entry = {
        .binding = 0,
        .buffer = r->surf.compute.unpack_src,
        .offset = 0,
        .size = data_size,
    };
    WGPUBindGroup bg = wgpuDeviceCreateBindGroup(
        r->device, &(WGPUBindGroupDescriptor){
                       .layout = r->surf.compute.unpack_bgl,
                       .entryCount = 1,
                       .entries = &entry });

    WGPURenderPassDepthStencilAttachment dsa = {
        .view = surface->view,
        .depthLoadOp = WGPULoadOp_Load,
        .depthStoreOp = WGPUStoreOp_Store,
        .stencilLoadOp = WGPULoadOp_Load,
        .stencilStoreOp = WGPUStoreOp_Store,
    };
    WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(
        enc, &(WGPURenderPassDescriptor){
                 .label = { "zeta unpack", WGPU_STRLEN },
                 .depthStencilAttachment = &dsa });
    wgpuRenderPassEncoderSetViewport(pass, 0, 0, width, height, 0.0f, 1.0f);
    wgpuRenderPassEncoderSetScissorRect(pass, 0, 0, width, height);
    wgpuRenderPassEncoderSetPipeline(pass, r->surf.compute.unpack_pipeline);
    wgpuRenderPassEncoderSetBindGroup(pass, 0, bg, 0, NULL);
    wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
    wgpuRenderPassEncoderEnd(pass);
    wgpuRenderPassEncoderRelease(pass);
    wgpuBindGroupRelease(bg);
}

/* A CopySrc buffer holding @size bytes of @data (released by the caller). */
static WGPUBuffer upload_temp_buffer(PGRAPHWgpuState *r, const void *data,
                                     size_t size)
{
    WGPUBuffer b = wgpuDeviceCreateBuffer(
        r->device, &(WGPUBufferDescriptor){
                       .label = { "zeta xfer temp", WGPU_STRLEN },
                       .usage = WGPUBufferUsage_CopySrc |
                                WGPUBufferUsage_CopyDst,
                       .size = ROUND_UP(size, 4) });
    wgpuQueueWriteBuffer(r->queue, b, 0, data, ROUND_UP(size, 4));
    return b;
}

/*
 * As pgraph_wgpu_unpack_depth_stencil, but the width x height packed Z24S8
 * words (tight rows) are already in GPU buffer @src at @src_offset (needs
 * CopySrc usage): no CPU round trip.
 */
void pgraph_wgpu_unpack_depth_stencil_gpu(PGRAPHState *pg,
                                          SurfaceBinding *surface,
                                          WGPUCommandEncoder enc,
                                          WGPUBuffer src, size_t src_offset)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;

    assert(surface->host_fmt.conv == WGPU_SURFACE_CONV_Z24S8);

    unsigned int width = surface->width, height = surface->height;
    size_t num_pixels = (size_t)width * height;
    size_t data_size = UNPACK_HEADER_SIZE + num_pixels * 4;
    size_t stride = ROUND_UP(width, 256);   /* stencil bytes per row */

    ensure_buffer(r, &r->surf.compute.unpack_src,
                  &r->surf.compute.unpack_src_size, data_size,
                  WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst,
                  "zeta unpack src");
    ensure_buffer(r, &r->surf.compute.stencil_dst,
                  &r->surf.compute.stencil_dst_size, stride * height,
                  WGPUBufferUsage_Storage | WGPUBufferUsage_CopySrc,
                  "zeta stencil dst");
    /*
     * The header goes through a copy recorded in @enc, not a queue write: a
     * queue write would land before earlier unpacks recorded in the same,
     * not yet submitted, encoder.
     */
    uint32_t header[UNPACK_HEADER_SIZE / 4] = { width, height, stride / 4, 0 };
    WGPUBuffer hdr = upload_temp_buffer(r, header, sizeof(header));
    wgpuCommandEncoderCopyBufferToBuffer(enc, hdr, 0,
                                         r->surf.compute.unpack_src, 0,
                                         sizeof(header));
    wgpuBufferRelease(hdr);
    wgpuCommandEncoderCopyBufferToBuffer(enc, src, src_offset,
                                         r->surf.compute.unpack_src,
                                         UNPACK_HEADER_SIZE, num_pixels * 4);

    /* stencil: bits 7-0 of each word into padded rows, then a copy */
    WGPUBindGroupEntry entries[2] = {
        { .binding = 0, .buffer = r->surf.compute.unpack_src, .offset = 0,
          .size = data_size },
        { .binding = 1, .buffer = r->surf.compute.stencil_dst, .offset = 0,
          .size = stride * height },
    };
    WGPUBindGroup bg = wgpuDeviceCreateBindGroup(
        r->device, &(WGPUBindGroupDescriptor){
                       .layout = r->surf.compute.stencil_bgl,
                       .entryCount = ARRAY_SIZE(entries),
                       .entries = entries });
    WGPUComputePassEncoder pass = wgpuCommandEncoderBeginComputePass(
        enc, &(WGPUComputePassDescriptor){
                 .label = { "zeta stencil extract", WGPU_STRLEN } });
    wgpuComputePassEncoderSetPipeline(pass, r->surf.compute.stencil_pipeline);
    wgpuComputePassEncoderSetBindGroup(pass, 0, bg, 0, NULL);
    wgpuComputePassEncoderDispatchWorkgroups(
        pass, DIV_ROUND_UP(DIV_ROUND_UP(width, 4), 64), height, 1);
    wgpuComputePassEncoderEnd(pass);
    wgpuComputePassEncoderRelease(pass);
    wgpuBindGroupRelease(bg);

    WGPUTexelCopyBufferInfo from = {
        .layout = { .offset = 0, .bytesPerRow = stride, .rowsPerImage = height },
        .buffer = r->surf.compute.stencil_dst,
    };
    WGPUTexelCopyTextureInfo to = {
        .texture = surface->texture,
        .aspect = WGPUTextureAspect_StencilOnly,
    };
    wgpuCommandEncoderCopyBufferToTexture(enc, &from, &to,
                                          &(WGPUExtent3D){ width, height, 1 });

    unpack_depth_pass(r, surface, enc, data_size);
}

void pgraph_wgpu_unpack_depth_stencil(PGRAPHState *pg, SurfaceBinding *surface,
                                      WGPUCommandEncoder enc,
                                      const uint32_t *z24s8)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;

    assert(surface->host_fmt.conv == WGPU_SURFACE_CONV_Z24S8);

    /* FIXME: surface scaling (pgraph_apply_scaling_factor) not supported */
    unsigned int width = surface->width, height = surface->height;
    size_t num_pixels = (size_t)width * height;

    //
    // Stencil: bits 7-0, written directly into the stencil aspect
    //

    g_autofree uint8_t *stencil = g_malloc(num_pixels);
    for (size_t i = 0; i < num_pixels; i++) {
        stencil[i] = z24s8[i] & 0xff;
    }
    WGPUTexelCopyTextureInfo dst = {
        .texture = surface->texture,
        .aspect = WGPUTextureAspect_StencilOnly,
    };
    WGPUTexelCopyBufferLayout layout = {
        .bytesPerRow = width,
        .rowsPerImage = height,
    };
    wgpuQueueWriteTexture(r->queue, &dst, stencil, num_pixels, &layout,
                          &(WGPUExtent3D){ width, height, 1 });

    //
    // Depth: bits 31-8, via a full-screen pass writing frag_depth
    //

    size_t data_size = UNPACK_HEADER_SIZE + num_pixels * 4;
    ensure_buffer(r, &r->surf.compute.unpack_src,
                  &r->surf.compute.unpack_src_size, data_size,
                  WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst,
                  "zeta unpack src");
    uint32_t header[UNPACK_HEADER_SIZE / 4] = { width, height, 0, 0 };
    wgpuQueueWriteBuffer(r->queue, r->surf.compute.unpack_src, 0, header,
                         sizeof(header));
    wgpuQueueWriteBuffer(r->queue, r->surf.compute.unpack_src,
                         UNPACK_HEADER_SIZE, z24s8, num_pixels * 4);

    unpack_depth_pass(r, surface, enc, data_size);
}

/*
 * Build Z24S8 surface @zeta from the bytes of linear 32bpp color surface
 * @color at the same address and pitch (pitch == width * 4, a multiple of
 * 256): its first color->height rows come from the color texture on the GPU,
 * the remaining rows from @tail (guest layout, pitch-strided VRAM).
 */
void pgraph_wgpu_zeta_from_color(PGRAPHState *pg, SurfaceBinding *zeta,
                                 SurfaceBinding *color,
                                 WGPUCommandEncoder enc, const uint8_t *tail)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;
    size_t row = (size_t)zeta->width * 4;
    unsigned int head_rows = MIN(color->height, zeta->height);
    size_t total = row * zeta->height;

    assert(row == zeta->pitch && row % 256 == 0 && color->width == zeta->width);
    ensure_buffer(r, &r->surf.compute.xfer, &r->surf.compute.xfer_size, total,
                  WGPUBufferUsage_CopySrc | WGPUBufferUsage_CopyDst,
                  "zeta xfer");
    WGPUTexelCopyTextureInfo from = {
        .texture = color->texture, .aspect = WGPUTextureAspect_All,
    };
    WGPUTexelCopyBufferInfo to = {
        .layout = { .offset = 0, .bytesPerRow = row,
                    .rowsPerImage = head_rows },
        .buffer = r->surf.compute.xfer,
    };
    wgpuCommandEncoderCopyTextureToBuffer(
        enc, &from, &to, &(WGPUExtent3D){ zeta->width, head_rows, 1 });
    if (zeta->height > head_rows) {
        size_t tail_size = row * (zeta->height - head_rows);
        WGPUBuffer t = upload_temp_buffer(r, tail, tail_size);
        wgpuCommandEncoderCopyBufferToBuffer(enc, t, 0, r->surf.compute.xfer,
                                             row * head_rows, tail_size);
        wgpuBufferRelease(t);
    }
    pgraph_wgpu_unpack_depth_stencil_gpu(pg, zeta, enc, r->surf.compute.xfer, 0);
}
