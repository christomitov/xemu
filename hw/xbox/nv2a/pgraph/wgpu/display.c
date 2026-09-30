/*
 * Geforce NV2A PGRAPH WebGPU Renderer: present the CRTC scanout to the canvas
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#include "renderer.h"
#include <emscripten.h>

static const char display_wgsl[] =
    "@group(0) @binding(0) var samp: sampler;\n"
    "@group(0) @binding(1) var tex: texture_2d<f32>;\n"
    "struct VOut { @builtin(position) pos: vec4f, @location(0) uv: vec2f };\n"
    "@vertex fn vs(@builtin(vertex_index) i: u32) -> VOut {\n"
    "    var p = array(vec2f(-1.0, -1.0), vec2f(3.0, -1.0), vec2f(-1.0, 3.0));\n"
    "    var o: VOut;\n"
    "    o.pos = vec4f(p[i], 0.0, 1.0);\n"
    "    o.uv = vec2f((p[i].x + 1.0) * 0.5, (1.0 - p[i].y) * 0.5);\n"
    "    return o;\n"
    "}\n"
    "@fragment fn fs(v: VOut) -> @location(0) vec4f {\n"
    "    return vec4f(textureSample(tex, samp, v.uv).rgb, 1.0);\n"
    "}\n";

void pgraph_wgpu_init_display(PGRAPHState *pg)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;
    PGRAPHWgpuDisplayState *disp = &r->display;

    WGPUShaderModule module =
        pgraph_wgpu_create_wgsl_module(r, "display", display_wgsl);

    WGPUBindGroupLayoutEntry entries[2] = {
        { .binding = 0, .visibility = WGPUShaderStage_Fragment,
          .sampler = { .type = WGPUSamplerBindingType_Filtering } },
        { .binding = 1, .visibility = WGPUShaderStage_Fragment,
          .texture = { .sampleType = WGPUTextureSampleType_Float,
                       .viewDimension = WGPUTextureViewDimension_2D } },
    };
    disp->bind_group_layout = wgpuDeviceCreateBindGroupLayout(
        r->device, &(WGPUBindGroupLayoutDescriptor){ .entryCount = 2,
                                                     .entries = entries });
    WGPUPipelineLayout layout = wgpuDeviceCreatePipelineLayout(
        r->device, &(WGPUPipelineLayoutDescriptor){
                       .bindGroupLayoutCount = 1,
                       .bindGroupLayouts = &disp->bind_group_layout });

    WGPUColorTargetState target = {
        .format = r->surface_format,
        .writeMask = WGPUColorWriteMask_All,
    };
    WGPUFragmentState frag = {
        .module = module,
        .entryPoint = { "fs", WGPU_STRLEN },
        .targetCount = 1,
        .targets = &target,
    };
    WGPURenderPipelineDescriptor pdesc = {
        .label = { "display", WGPU_STRLEN },
        .layout = layout,
        .vertex = { .module = module, .entryPoint = { "vs", WGPU_STRLEN } },
        .primitive = { .topology = WGPUPrimitiveTopology_TriangleList },
        .multisample = { .count = 1, .mask = ~0u },
        .fragment = &frag,
    };
    disp->pipeline = wgpuDeviceCreateRenderPipeline(r->device, &pdesc);
    wgpuPipelineLayoutRelease(layout);
    wgpuShaderModuleRelease(module);

    disp->sampler = wgpuDeviceCreateSampler(
        r->device, &(WGPUSamplerDescriptor){
                       .addressModeU = WGPUAddressMode_ClampToEdge,
                       .addressModeV = WGPUAddressMode_ClampToEdge,
                       .addressModeW = WGPUAddressMode_ClampToEdge,
                       .magFilter = WGPUFilterMode_Linear,
                       .minFilter = WGPUFilterMode_Linear,
                       .mipmapFilter = WGPUMipmapFilterMode_Nearest,
                       .lodMaxClamp = 32.0f,
                       .maxAnisotropy = 1 });
}

static void release_source(PGRAPHWgpuDisplayState *disp)
{
    if (disp->bind_group) {
        wgpuBindGroupRelease(disp->bind_group);
        disp->bind_group = NULL;
    }
    if (disp->view) {
        wgpuTextureViewRelease(disp->view);
        disp->view = NULL;
    }
    if (disp->texture) {
        wgpuTextureDestroy(disp->texture);
        wgpuTextureRelease(disp->texture);
        disp->texture = NULL;
    }
}

void pgraph_wgpu_finalize_display(PGRAPHState *pg)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;
    PGRAPHWgpuDisplayState *disp = &r->display;

    release_source(disp);
    g_free(disp->conv);
    if (disp->sampler) {
        wgpuSamplerRelease(disp->sampler);
    }
    if (disp->pipeline) {
        wgpuRenderPipelineRelease(disp->pipeline);
    }
    if (disp->bind_group_layout) {
        wgpuBindGroupLayoutRelease(disp->bind_group_layout);
    }
}

static void ensure_source(PGRAPHWgpuState *r, int width, int height)
{
    PGRAPHWgpuDisplayState *disp = &r->display;

    if (disp->texture && disp->width == width && disp->height == height) {
        return;
    }
    release_source(disp);

    disp->texture = wgpuDeviceCreateTexture(
        r->device, &(WGPUTextureDescriptor){
                       .label = { "scanout", WGPU_STRLEN },
                       .usage = WGPUTextureUsage_TextureBinding |
                                WGPUTextureUsage_CopyDst,
                       .dimension = WGPUTextureDimension_2D,
                       .size = { width, height, 1 },
                       .format = WGPUTextureFormat_BGRA8Unorm,
                       .mipLevelCount = 1,
                       .sampleCount = 1 });
    disp->view = wgpuTextureCreateView(disp->texture, NULL);
    WGPUBindGroupEntry entries[2] = {
        { .binding = 0, .sampler = disp->sampler },
        { .binding = 1, .textureView = disp->view },
    };
    disp->bind_group = wgpuDeviceCreateBindGroup(
        r->device, &(WGPUBindGroupDescriptor){
                       .layout = disp->bind_group_layout,
                       .entryCount = 2,
                       .entries = entries });
    disp->width = width;
    disp->height = height;

    if (r->surface_width != width || r->surface_height != height) {
        WGPUSurfaceConfiguration cfg = {
            .device = r->device,
            .format = r->surface_format,
            .usage = WGPUTextureUsage_RenderAttachment,
            .width = width,
            .height = height,
            .alphaMode = WGPUCompositeAlphaMode_Opaque,
            .presentMode = WGPUPresentMode_Fifo,
        };
        wgpuSurfaceConfigure(r->surface, &cfg);
        r->surface_width = width;
        r->surface_height = height;
    }
}

/*
 * Upload the CRTC scanout from VRAM. Mirrors nv2a_get_bpp() and the VGA
 * line-offset decode; 15/16bpp is expanded to BGRA8 on the CPU.
 * Returns false if no valid 15/16/32bpp mode is set yet.
 */
static bool upload_scanout(NV2AState *d, PGRAPHWgpuState *r)
{
    VGACommonState *v = &d->vga;
    PGRAPHWgpuDisplayState *disp = &r->display;

    int depth = v->cr[0x28] & 3;
    int bpp;
    switch (depth) {
    case 2:
        bpp = (d->pramdac.general_control &
               NV_PRAMDAC_GENERAL_CONTROL_ALT_MODE_SEL) ? 16 : 15;
        break;
    case 3:
        bpp = 32;
        break;
    default:
        return false;
    }
    int bytes_pp = (bpp + 7) / 8;
    uint32_t pitch = (v->cr[0x13] | ((v->cr[0x19] & 0xe0) << 3) |
                      ((v->cr[0x25] & 0x20) << 6)) << 3;
    if (pitch == 0 || pitch > 1920 * 4) {
        return false;
    }

    VGADisplayParams params;
    d->vga.get_params(&d->vga, &params);
    int width = pitch / bytes_pp;
    int height = 480;
    if (width > 1024) {
        height = 720; /* 1280x720 HD modes */
    }
    hwaddr base = d->pcrtc.start;
    if (base + (hwaddr)pitch * height > memory_region_size(d->vram)) {
        return false;
    }
    const uint8_t *src = d->vram_ptr + base;

    ensure_source(r, width, height);

    const void *data = src;
    uint32_t row_bytes = pitch;
    if (bytes_pp == 2) {
        size_t need = (size_t)width * height * 4;
        if (disp->conv_size < need) {
            disp->conv = g_realloc(disp->conv, need);
            disp->conv_size = need;
        }
        for (int y = 0; y < height; y++) {
            const uint16_t *s = (const uint16_t *)(src + y * pitch);
            uint32_t *o = disp->conv + (size_t)y * width;
            for (int x = 0; x < width; x++) {
                uint32_t p = s[x], rr, gg, bb;
                if (bpp == 16) {
                    rr = (p >> 11) & 0x1f; gg = (p >> 5) & 0x3f; bb = p & 0x1f;
                    rr = (rr << 3) | (rr >> 2);
                    gg = (gg << 2) | (gg >> 4);
                } else {
                    rr = (p >> 10) & 0x1f; gg = (p >> 5) & 0x1f; bb = p & 0x1f;
                    rr = (rr << 3) | (rr >> 2);
                    gg = (gg << 3) | (gg >> 2);
                }
                bb = (bb << 3) | (bb >> 2);
                o[x] = 0xff000000u | (rr << 16) | (gg << 8) | bb; /* BGRA */
            }
        }
        data = disp->conv;
        row_bytes = width * 4;
    }

    WGPUTexelCopyTextureInfo dst = { .texture = disp->texture };
    WGPUTexelCopyBufferLayout layout = { .bytesPerRow = row_bytes,
                                         .rowsPerImage = height };
    WGPUExtent3D size = { width, height, 1 };
    wgpuQueueWriteTexture(r->queue, &dst, data, (size_t)row_bytes * height,
                          &layout, &size);
    return true;
}

void pgraph_wgpu_render_display(NV2AState *d)
{
    PGRAPHWgpuState *r = d->pgraph.wgpu_renderer_state;
    PGRAPHWgpuDisplayState *disp = &r->display;

    if (!upload_scanout(d, r) || !disp->bind_group) {
        return;
    }

    WGPUSurfaceTexture st;
    wgpuSurfaceGetCurrentTexture(r->surface, &st);
    if (st.status != WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal &&
        st.status != WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal) {
        return;
    }
    WGPUTextureView target = wgpuTextureCreateView(st.texture, NULL);

    WGPURenderPassColorAttachment ca = {
        .view = target,
        .depthSlice = WGPU_DEPTH_SLICE_UNDEFINED,
        .loadOp = WGPULoadOp_Clear,
        .storeOp = WGPUStoreOp_Store,
        .clearValue = { 0, 0, 0, 1 },
    };
    WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(r->device, NULL);
    WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(
        enc, &(WGPURenderPassDescriptor){ .colorAttachmentCount = 1,
                                          .colorAttachments = &ca });
    wgpuRenderPassEncoderSetPipeline(pass, disp->pipeline);
    wgpuRenderPassEncoderSetBindGroup(pass, 0, disp->bind_group, 0, NULL);
    wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
    wgpuRenderPassEncoderEnd(pass);
    WGPUCommandBuffer cb = wgpuCommandEncoderFinish(enc, NULL);
    wgpuQueueSubmit(r->queue, 1, &cb);

    wgpuCommandBufferRelease(cb);
    wgpuRenderPassEncoderRelease(pass);
    wgpuCommandEncoderRelease(enc);
    wgpuTextureViewRelease(target);
    wgpuTextureRelease(st.texture);

    /* A worker's OffscreenCanvas only presents when its task ends: yield to
     * the event loop (Asyncify unwinds the pfifo thread) so this frame shows. */
    emscripten_sleep(0);

    static unsigned presented;
    if (++presented % 300 == 1) {
        fprintf(stderr, "[wgpu] presented %u frames (%dx%d)\n", presented,
                disp->width, disp->height);
    }
}
