/*
 * Geforce NV2A PGRAPH WebGPU Renderer: present the CRTC scanout to the canvas
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#include "renderer.h"
#include "qemu/xemu-wasm-stats.h"
#include "hw/xbox/nv2a/pgraph/util.h"
#include <emscripten.h>
#include <math.h>

/* Display uniforms, std140-compatible with DisplayU in display_wgsl. */
typedef struct DisplayUniforms {
    float xform[4];         /* uv scale (xy) / offset (zw) into the surface */
    float display_size[2];
    uint32_t pvideo_enable;
    uint32_t pvideo_color_key_enable;
    float pvideo_in_pos[2];
    float pad_[2];
    float pvideo_pos[4];    /* out x, y, w, h */
    float pvideo_scale[4];  /* in/out x, y, 1/surface scale, - */
    float pvideo_color_key[4];
} DisplayUniforms;

/* uv = xform.xy * screen_uv + xform.zw selects the scanned-out region of a
 * (possibly larger) render surface; the PVIDEO video overlay is composited
 * on top exactly like the Vulkan display shader (minus its GL y-flip) */
#define DISPLAY_WGSL(DISPLAY_AA_BODY) \
    "struct DisplayU {\n" \
    "    xform: vec4f,\n" \
    "    display_size: vec2f,\n" \
    "    pvideo_enable: u32,\n" \
    "    pvideo_color_key_enable: u32,\n" \
    "    pvideo_in_pos: vec2f,\n" \
    "    pad_: vec2f,\n" \
    "    pvideo_pos: vec4f,\n" \
    "    pvideo_scale: vec4f,\n" \
    "    pvideo_color_key: vec4f,\n" \
    "};\n" \
    "@group(0) @binding(0) var samp: sampler;\n" \
    "@group(0) @binding(1) var tex: texture_2d<f32>;\n" \
    "@group(0) @binding(2) var<uniform> u: DisplayU;\n" \
    "@group(0) @binding(3) var pvideo_tex: texture_2d<f32>;\n" \
    "struct VOut { @builtin(position) pos: vec4f, @location(0) uv: vec2f };\n" \
    "@vertex fn vs(@builtin(vertex_index) i: u32) -> VOut {\n" \
    "    var p = array(vec2f(-1.0, -1.0), vec2f(3.0, -1.0), vec2f(-1.0, 3.0));\n" \
    "    var o: VOut;\n" \
    "    o.pos = vec4f(p[i], 0.0, 1.0);\n" \
    "    o.uv = vec2f((p[i].x + 1.0) * 0.5, (1.0 - p[i].y) * 0.5);\n" \
    "    return o;\n" \
    "}\n" \
    "fn luma(c: vec3f) -> f32 { return dot(c, vec3f(0.299, 0.587, 0.114)); }\n" \
    "fn sample_base(uv: vec2f, raw: vec4f) -> vec4f {\n" \
    DISPLAY_AA_BODY \
    "}\n" \
    "@fragment fn fs(v: VOut) -> @location(0) vec4f {\n" \
    "    let uv = v.uv * u.xform.xy + u.xform.zw;\n" \
    "    let raw = textureSampleLevel(tex, samp, uv, 0.0);\n" \
    "    var c = sample_base(uv, raw);\n" \
    "    if (u.pvideo_enable != 0u) {\n" \
    "        let sc = v.uv * u.display_size * u.pvideo_scale.z;\n" \
    "        let lo = u.pvideo_pos.xy;\n" \
    "        let hi = u.pvideo_pos.xy + u.pvideo_pos.zw;\n" \
    "        let inside = all(sc >= lo) && all(sc <= hi);\n" \
    "        let keyed = u.pvideo_color_key_enable == 0u ||\n" \
    "                    all(raw.rgb == u.pvideo_color_key.rgb);\n" \
    "        if (inside && keyed) {\n" \
    "            let in_st = (u.pvideo_in_pos + (sc - lo) * u.pvideo_scale.xy)\n" \
    "                        / vec2f(textureDimensions(pvideo_tex, 0));\n" \
    "            c = textureSampleLevel(pvideo_tex, samp, in_st, 0.0);\n" \
    "        }\n" \
    "    }\n" \
    "    return vec4f(c.rgb, 1.0);\n" \
    "}\n"

/* FXAA 3.11 "console" style edge blend, in texture space */
#define DISPLAY_AA_FXAA \
    "    let rcp = 1.0 / vec2f(textureDimensions(tex, 0));\n" \
    "    let nw = luma(textureSampleLevel(tex, samp, uv + vec2f(-1.0, -1.0) * rcp, 0.0).rgb);\n" \
    "    let ne = luma(textureSampleLevel(tex, samp, uv + vec2f(1.0, -1.0) * rcp, 0.0).rgb);\n" \
    "    let sw = luma(textureSampleLevel(tex, samp, uv + vec2f(-1.0, 1.0) * rcp, 0.0).rgb);\n" \
    "    let se = luma(textureSampleLevel(tex, samp, uv + vec2f(1.0, 1.0) * rcp, 0.0).rgb);\n" \
    "    let m = luma(raw.rgb);\n" \
    "    let lmin = min(m, min(min(nw, ne), min(sw, se)));\n" \
    "    let lmax = max(m, max(max(nw, ne), max(sw, se)));\n" \
    "    if (lmax - lmin < max(0.0312, lmax * 0.125)) { return raw; }\n" \
    "    var dir = vec2f(-((nw + ne) - (sw + se)), (nw + sw) - (ne + se));\n" \
    "    let reduce = max((nw + ne + sw + se) * (0.25 / 8.0), 1.0 / 128.0);\n" \
    "    let rmin = 1.0 / (min(abs(dir.x), abs(dir.y)) + reduce);\n" \
    "    dir = clamp(dir * rmin, vec2f(-8.0), vec2f(8.0)) * rcp;\n" \
    "    let a = 0.5 * (textureSampleLevel(tex, samp, uv + dir * (1.0 / 3.0 - 0.5), 0.0).rgb +\n" \
    "                   textureSampleLevel(tex, samp, uv + dir * (2.0 / 3.0 - 0.5), 0.0).rgb);\n" \
    "    let b = a * 0.5 + 0.25 * (textureSampleLevel(tex, samp, uv - dir * 0.5, 0.0).rgb +\n" \
    "                              textureSampleLevel(tex, samp, uv + dir * 0.5, 0.0).rgb);\n" \
    "    let lb = luma(b);\n" \
    "    if (lb < lmin || lb > lmax) { return vec4f(a, raw.a); }\n" \
    "    return vec4f(b, raw.a);\n"
#define DISPLAY_AA_NONE "    return raw;\n"

/*
 * Catmull-Rom bicubic upscale in 9 bilinear taps: the canvas is sized to
 * the screen's physical pixels, so this (not the browser's bilinear
 * stretch) scales the 480p frame up — noticeably sharper.
 */
#define DISPLAY_AA_BICUBIC \
    "    let size = vec2f(textureDimensions(tex, 0));\n" \
    "    let sp = uv * size;\n" \
    "    let tp1 = floor(sp - 0.5) + 0.5;\n" \
    "    let f = sp - tp1;\n" \
    "    let w0 = f * (-0.5 + f * (1.0 - 0.5 * f));\n" \
    "    let w1 = 1.0 + f * f * (-2.5 + 1.5 * f);\n" \
    "    let w2 = f * (0.5 + f * (2.0 - 1.5 * f));\n" \
    "    let w3 = f * f * (-0.5 + 0.5 * f);\n" \
    "    let w12 = w1 + w2;\n" \
    "    let t0 = (tp1 - 1.0) / size;\n" \
    "    let t3 = (tp1 + 2.0) / size;\n" \
    "    let t12 = (tp1 + w2 / w12) / size;\n" \
    "    var r = vec3f(0.0);\n" \
    "    r += textureSampleLevel(tex, samp, vec2f(t0.x, t0.y), 0.0).rgb * w0.x * w0.y;\n" \
    "    r += textureSampleLevel(tex, samp, vec2f(t12.x, t0.y), 0.0).rgb * w12.x * w0.y;\n" \
    "    r += textureSampleLevel(tex, samp, vec2f(t3.x, t0.y), 0.0).rgb * w3.x * w0.y;\n" \
    "    r += textureSampleLevel(tex, samp, vec2f(t0.x, t12.y), 0.0).rgb * w0.x * w12.y;\n" \
    "    r += textureSampleLevel(tex, samp, vec2f(t12.x, t12.y), 0.0).rgb * w12.x * w12.y;\n" \
    "    r += textureSampleLevel(tex, samp, vec2f(t3.x, t12.y), 0.0).rgb * w3.x * w12.y;\n" \
    "    r += textureSampleLevel(tex, samp, vec2f(t0.x, t3.y), 0.0).rgb * w0.x * w3.y;\n" \
    "    r += textureSampleLevel(tex, samp, vec2f(t12.x, t3.y), 0.0).rgb * w12.x * w3.y;\n" \
    "    r += textureSampleLevel(tex, samp, vec2f(t3.x, t3.y), 0.0).rgb * w3.x * w3.y;\n" \
    "    return vec4f(clamp(r, vec3f(0.0), vec3f(1.0)), raw.a);\n"

static const char display_wgsl_fxaa[] = DISPLAY_WGSL(DISPLAY_AA_FXAA);
static const char display_wgsl_bicubic[] = DISPLAY_WGSL(DISPLAY_AA_BICUBIC);
static const char display_wgsl[] = DISPLAY_WGSL(DISPLAY_AA_NONE);

static void ensure_pvideo_texture(PGRAPHWgpuState *r, int width, int height)
{
    PGRAPHWgpuDisplayState *disp = &r->display;

    if (disp->pvideo_texture && disp->pvideo_width == width &&
        disp->pvideo_height == height) {
        return;
    }
    if (disp->pvideo_view) {
        wgpuTextureViewRelease(disp->pvideo_view);
    }
    if (disp->pvideo_texture) {
        wgpuTextureRelease(disp->pvideo_texture);
    }
    disp->pvideo_texture = wgpuDeviceCreateTexture(
        r->device, &(WGPUTextureDescriptor){
                       .label = { "pvideo", WGPU_STRLEN },
                       .usage = WGPUTextureUsage_TextureBinding |
                                WGPUTextureUsage_CopyDst,
                       .dimension = WGPUTextureDimension_2D,
                       .size = { width, height, 1 },
                       .format = WGPUTextureFormat_RGBA8Unorm,
                       .mipLevelCount = 1,
                       .sampleCount = 1 });
    disp->pvideo_view = wgpuTextureCreateView(disp->pvideo_texture, NULL);
    disp->pvideo_width = width;
    disp->pvideo_height = height;
}

static float pvideo_calculate_scale(unsigned int din_dout,
                                    unsigned int output_size)
{
    float calculated_in = din_dout * (output_size - 1);
    calculated_in = floorf(calculated_in / (1 << 20) + 0.5f);
    return (calculated_in + 1.0f) / output_size;
}

/*
 * PVIDEO overlay state, as in the Vulkan backend's get_pvideo_state(), but
 * unsupported formats / out-of-range buffers disable the overlay instead of
 * asserting. Fills the overlay part of *u and uploads the frame (YUY2 ->
 * RGBA on the CPU) when enabled.
 */
static void update_pvideo(NV2AState *d, PGRAPHWgpuState *r, DisplayUniforms *u)
{
    PGRAPHWgpuDisplayState *disp = &r->display;
    uint32_t *regs = d->pvideo.regs;

    u->pvideo_enable = (regs[NV_PVIDEO_BUFFER] & NV_PVIDEO_BUFFER_0_USE) &&
                       regs[NV_PVIDEO_SIZE_IN] != 0xFFFFFFFF;
    if (!u->pvideo_enable) {
        return;
    }

    hwaddr base = regs[NV_PVIDEO_BASE];
    hwaddr limit = regs[NV_PVIDEO_LIMIT];
    hwaddr offset = regs[NV_PVIDEO_OFFSET];
    unsigned int pitch = GET_MASK(regs[NV_PVIDEO_FORMAT], NV_PVIDEO_FORMAT_PITCH);
    unsigned int format =
        GET_MASK(regs[NV_PVIDEO_FORMAT], NV_PVIDEO_FORMAT_COLOR);
    unsigned int in_w = GET_MASK(regs[NV_PVIDEO_SIZE_IN], NV_PVIDEO_SIZE_IN_WIDTH);
    unsigned int in_h = GET_MASK(regs[NV_PVIDEO_SIZE_IN], NV_PVIDEO_SIZE_IN_HEIGHT);
    unsigned int out_w =
        GET_MASK(regs[NV_PVIDEO_SIZE_OUT], NV_PVIDEO_SIZE_OUT_WIDTH);
    unsigned int out_h =
        GET_MASK(regs[NV_PVIDEO_SIZE_OUT], NV_PVIDEO_SIZE_OUT_HEIGHT);
    uint32_t ds_dx = regs[NV_PVIDEO_DS_DX];
    uint32_t dt_dy = regs[NV_PVIDEO_DT_DY];
    float scale_x = ds_dx == NV_PVIDEO_DIN_DOUT_UNITY ?
                        1.0f : pvideo_calculate_scale(ds_dx, out_w);
    float scale_y = dt_dy == NV_PVIDEO_DIN_DOUT_UNITY ?
                        1.0f : pvideo_calculate_scale(dt_dy, out_h);

    /* HW caps SIZE_IN to SIZE_OUT without scaling (see vk display.c) */
    if (in_w > out_w) {
        in_w = floorf((float)out_w * scale_x + 0.5f);
    }
    if (in_h > out_h) {
        in_h = floorf((float)out_h * scale_y + 0.5f);
    }

    if (format != NV_PVIDEO_FORMAT_COLOR_LE_CR8YB8CB8YA8 || !in_w || !in_h ||
        !pitch || offset + (hwaddr)pitch * in_h > limit ||
        base + offset + (hwaddr)pitch * in_h > memory_region_size(d->vram)) {
        static bool logged;
        if (!logged) {
            fprintf(stderr, "[wgpu] pvideo overlay not shown: format=%u "
                    "%ux%u pitch=%u\n", format, in_w, in_h, pitch);
            logged = true;
        }
        u->pvideo_enable = 0;
        return;
    }

    ensure_pvideo_texture(r, in_w, in_h);
    size_t need = (size_t)in_w * in_h * 4;
    if (disp->pvideo_conv_size < need) {
        disp->pvideo_conv = g_realloc(disp->pvideo_conv, need);
        disp->pvideo_conv_size = need;
    }
    const uint8_t *src = d->vram_ptr + base + offset;
    uint8_t *out = disp->pvideo_conv;
    for (unsigned int y = 0; y < in_h; y++) {
        const uint8_t *line = src + (size_t)y * pitch;
        uint8_t *px = out + (size_t)y * in_w * 4;
        for (unsigned int x = 0; x < in_w; x++, px += 4) {
            convert_yuy2_to_rgb(line, x, &px[0], &px[1], &px[2]);
            px[3] = 255;
        }
    }
    wgpuQueueWriteTexture(
        r->queue, &(WGPUTexelCopyTextureInfo){ .texture = disp->pvideo_texture },
        out, need,
        &(WGPUTexelCopyBufferLayout){ .bytesPerRow = in_w * 4,
                                      .rowsPerImage = in_h },
        &(WGPUExtent3D){ in_w, in_h, 1 });

    uint32_t key = regs[NV_PVIDEO_COLOR_KEY] & 0xFFFFFF; /* ignores alpha */
    u->pvideo_color_key_enable =
        GET_MASK(regs[NV_PVIDEO_FORMAT], NV_PVIDEO_FORMAT_DISPLAY);
    u->pvideo_color_key[0] = GET_MASK(key, NV_PVIDEO_COLOR_KEY_RED) / 255.0f;
    u->pvideo_color_key[1] = GET_MASK(key, NV_PVIDEO_COLOR_KEY_GREEN) / 255.0f;
    u->pvideo_color_key[2] = GET_MASK(key, NV_PVIDEO_COLOR_KEY_BLUE) / 255.0f;
    u->pvideo_in_pos[0] =
        GET_MASK(regs[NV_PVIDEO_POINT_IN], NV_PVIDEO_POINT_IN_S) / 16.0f;
    u->pvideo_in_pos[1] =
        GET_MASK(regs[NV_PVIDEO_POINT_IN], NV_PVIDEO_POINT_IN_T) / 8.0f;
    u->pvideo_pos[0] =
        GET_MASK(regs[NV_PVIDEO_POINT_OUT], NV_PVIDEO_POINT_OUT_X);
    u->pvideo_pos[1] =
        GET_MASK(regs[NV_PVIDEO_POINT_OUT], NV_PVIDEO_POINT_OUT_Y);
    u->pvideo_pos[2] = out_w;
    u->pvideo_pos[3] = out_h;
    u->pvideo_scale[0] = scale_x;
    u->pvideo_scale[1] = scale_y;
    u->pvideo_scale[2] = 1.0f / d->pgraph.surface_scale_factor;
    u->pvideo_scale[3] = 1.0f;
}

static void on_display_scope(WGPUPopErrorScopeStatus status,
                             WGPUErrorType type, WGPUStringView msg,
                             void *ud1, void *ud2)
{
    (void)status;
    (void)ud2;
    if (type != WGPUErrorType_NoError) {
        *(bool *)ud1 = true;
        fprintf(stderr, "[wgpu] display FXAA shader rejected: %.*s\n",
                (int)(msg.length == WGPU_STRLEN ? strlen(msg.data) : msg.length),
                msg.data ? msg.data : "");
    }
}

static WGPURenderPipeline create_display_pipeline(PGRAPHWgpuState *r,
                                                  WGPUPipelineLayout layout,
                                                  const char *wgsl,
                                                  bool checked)
{
    bool failed = false;

    if (checked) {
        wgpuDevicePushErrorScope(r->device, WGPUErrorFilter_Validation);
    }
    WGPUShaderModule module =
        pgraph_wgpu_create_wgsl_module(r, "display", wgsl);
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
    WGPURenderPipeline pipeline =
        wgpuDeviceCreateRenderPipeline(r->device, &pdesc);
    wgpuShaderModuleRelease(module);

    if (checked) {
        pgraph_wgpu_wait(r, wgpuDevicePopErrorScope(
            r->device,
            (WGPUPopErrorScopeCallbackInfo){
                .mode = WGPUCallbackMode_WaitAnyOnly,
                .callback = on_display_scope,
                .userdata1 = &failed }));
        if (failed) {
            wgpuRenderPipelineRelease(pipeline);
            return NULL;
        }
    }
    return pipeline;
}

void pgraph_wgpu_init_display(PGRAPHState *pg)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;
    PGRAPHWgpuDisplayState *disp = &r->display;

    WGPUBindGroupLayoutEntry entries[4] = {
        { .binding = 0, .visibility = WGPUShaderStage_Fragment,
          .sampler = { .type = WGPUSamplerBindingType_Filtering } },
        { .binding = 1, .visibility = WGPUShaderStage_Fragment,
          .texture = { .sampleType = WGPUTextureSampleType_Float,
                       .viewDimension = WGPUTextureViewDimension_2D } },
        { .binding = 2, .visibility = WGPUShaderStage_Fragment,
          .buffer = { .type = WGPUBufferBindingType_Uniform,
                      .minBindingSize = sizeof(DisplayUniforms) } },
        { .binding = 3, .visibility = WGPUShaderStage_Fragment,
          .texture = { .sampleType = WGPUTextureSampleType_Float,
                       .viewDimension = WGPUTextureViewDimension_2D } },
    };
    disp->bind_group_layout = wgpuDeviceCreateBindGroupLayout(
        r->device, &(WGPUBindGroupLayoutDescriptor){ .entryCount = 4,
                                                     .entries = entries });
    WGPUPipelineLayout layout = wgpuDeviceCreatePipelineLayout(
        r->device, &(WGPUPipelineLayoutDescriptor){
                       .bindGroupLayoutCount = 1,
                       .bindGroupLayouts = &disp->bind_group_layout });

    /*
     * Upscale filter: bicubic by default, XEMU_AA=1 = FXAA (bilinear),
     * XEMU_FILTER=bilinear = plain; plain if the chosen shader fails.
     */
    const char *aa = getenv("XEMU_AA");
    const char *filter = getenv("XEMU_FILTER");
    disp->pipeline = NULL;
    if (aa && !strcmp(aa, "1")) {
        disp->pipeline = create_display_pipeline(r, layout, display_wgsl_fxaa,
                                                 true);
    } else if (!filter || strcmp(filter, "bilinear")) {
        disp->pipeline = create_display_pipeline(r, layout,
                                                 display_wgsl_bicubic, true);
    }
    if (!disp->pipeline) {
        disp->pipeline = create_display_pipeline(r, layout, display_wgsl,
                                                 false);
    }
    wgpuPipelineLayoutRelease(layout);

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

    disp->xform = wgpuDeviceCreateBuffer(
        r->device, &(WGPUBufferDescriptor){
                       .label = { "display uniforms", WGPU_STRLEN },
                       .usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst,
                       .size = sizeof(DisplayUniforms) });

    /* 1x1 black stand-in until a video overlay is shown */
    ensure_pvideo_texture(r, 1, 1);
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
    if (disp->surface_bind_group) {
        wgpuBindGroupRelease(disp->surface_bind_group);
    }
    if (disp->xform) {
        wgpuBufferDestroy(disp->xform);
        wgpuBufferRelease(disp->xform);
    }
    g_free(disp->conv);
    g_free(disp->pvideo_conv);
    if (disp->pvideo_view) {
        wgpuTextureViewRelease(disp->pvideo_view);
    }
    if (disp->pvideo_texture) {
        wgpuTextureRelease(disp->pvideo_texture);
    }
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

static WGPUBindGroup create_bind_group(PGRAPHWgpuState *r,
                                       WGPUTextureView view)
{
    PGRAPHWgpuDisplayState *disp = &r->display;
    WGPUBindGroupEntry entries[4] = {
        { .binding = 0, .sampler = disp->sampler },
        { .binding = 1, .textureView = view },
        { .binding = 2, .buffer = disp->xform, .size = sizeof(DisplayUniforms) },
        { .binding = 3, .textureView = disp->pvideo_view },
    };
    return wgpuDeviceCreateBindGroup(
        r->device, &(WGPUBindGroupDescriptor){
                       .layout = disp->bind_group_layout,
                       .entryCount = 4,
                       .entries = entries });
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
    disp->bind_group = create_bind_group(r, disp->view);
    disp->width = width;
    disp->height = height;
}

/* physical pixel size of the canvas on the page (0 = unknown), set by the
 * page via xemu_wasm_set_display_size() on load/resize */
volatile int xemu_wasm_display_w, xemu_wasm_display_h;

/* guest display size (scaled surface pixels), for the overlay mapping */
static int guest_display_w, guest_display_h;

static void configure_canvas(PGRAPHWgpuState *r, int width, int height)
{
    guest_display_w = width;
    guest_display_h = height;
    if (xemu_wasm_display_w > 0 && xemu_wasm_display_h > 0) {
        width = MIN(xemu_wasm_display_w, 3840);
        height = MIN(xemu_wasm_display_h, 2400);
    }
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
    configure_canvas(r, width, height);
    disp->xform_values[0] = 1.0f;
    disp->xform_values[1] = 1.0f;
    disp->xform_values[2] = 0.0f;
    disp->xform_values[3] = 0.0f;

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

/*
 * Present straight from the render surface that covers the CRTC start
 * address (no VRAM round trip), like the Vulkan backend's display path.
 */
static bool bind_render_surface(NV2AState *d, PGRAPHWgpuState *r,
                                WGPUBindGroup *bg)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHWgpuDisplayState *disp = &r->display;

    VGADisplayParams vga_display_params;
    d->vga.get_params(&d->vga, &vga_display_params);

    SurfaceBinding *surface = pgraph_wgpu_surface_get_within(
        d, d->pcrtc.start + vga_display_params.line_offset);
    if (surface == NULL || !surface->color || !surface->width ||
        !surface->height || !surface->view) {
        return false;
    }

    int width = 0, height = 0;
    d->vga.get_resolution(&d->vga, &width, &height);
    if (d->vga.cr[NV_PRMCIO_INTERLACE_MODE] !=
        NV_PRMCIO_INTERLACE_MODE_DISABLED) {
        height *= 2;
    }
    if (width <= 0 || height <= 0) {
        return false;
    }

    /*
     * Nothing new to show? Skip the whole present (finish + draw + yield,
     * ~5 ms): the canvas keeps the last frame. Anything that can change the
     * scanned-out image bumps one of these: draws (draw_time), flips
     * (frame_time), CPU writes (upload_pending), scanout address, overlay.
     */
    bool pvideo_on = (d->pvideo.regs[NV_PVIDEO_BUFFER] & NV_PVIDEO_BUFFER_0_USE);
    if (surface == disp->last_surface && pg->draw_time == disp->last_draw_time &&
        pg->frame_time == disp->last_frame_time &&
        d->pcrtc.start == disp->last_scanout && !surface->upload_pending &&
        !pvideo_on &&
        (xemu_wasm_display_w <= 0 || (r->surface_width == MIN(xemu_wasm_display_w, 3840) &&
                                      r->surface_height == MIN(xemu_wasm_display_h, 2400)))) {
        XSTAT_INC(n_present_skipped);
        *bg = NULL;
        return true;
    }
    disp->last_surface = surface;
    disp->last_draw_time = pg->draw_time;
    disp->last_frame_time = pg->frame_time;
    disp->last_scanout = d->pcrtc.start;

    /* render work for this surface may still be in the open encoder */
    pgraph_wgpu_finish(pg, WGPU_FINISH_REASON_PRESENTING);
    pgraph_wgpu_upload_surface_data(d, surface, false);

    unsigned int sw = surface->width, sh = surface->height;
    pgraph_apply_scaling_factor(pg, &sw, &sh);
    unsigned int dw = width, dh = height;
    pgraph_apply_scaling_factor(pg, &dw, &dh);
    configure_canvas(r, dw, dh);

    /* same mapping as the Vulkan display shader (minus its GL-compat
     * flip): whole surface across, vertical scale by display/texture
     * height and the CRTC line-offset ratio */
    int line_ratio = vga_display_params.line_offset ?
                         surface->pitch / vga_display_params.line_offset : 1;
    if (line_ratio <= 0) {
        line_ratio = 1;
    }
    disp->xform_values[0] = 1.0f;
    disp->xform_values[1] = (float)dh / (float)sh / (float)line_ratio;
    disp->xform_values[2] = 0.0f;
    disp->xform_values[3] = 0.0f;

    /* fresh bind group per frame: surfaces (and their views) come and go */
    if (disp->surface_bind_group) {
        wgpuBindGroupRelease(disp->surface_bind_group);
    }
    disp->surface_bind_group = create_bind_group(r, surface->view);
    disp->surface_bound_view = surface->view;
    surface->frame_time = pg->frame_time;
    *bg = disp->surface_bind_group;
    return true;
}

void pgraph_wgpu_render_display(NV2AState *d)
{
    XSTAT_T0();
    PGRAPHWgpuState *r = d->pgraph.wgpu_renderer_state;
    PGRAPHWgpuDisplayState *disp = &r->display;
    WGPUBindGroup bg = NULL;

    if (getenv("XEMU_VRAM_DISPLAY")) {
        /* debug: read the rendered surface back into VRAM, show VRAM */
        VGADisplayParams p;
        d->vga.get_params(&d->vga, &p);
        SurfaceBinding *s = pgraph_wgpu_surface_get_within(
            d, d->pcrtc.start + p.line_offset);
        if (s && s->color) {
            pgraph_wgpu_finish(&d->pgraph, WGPU_FINISH_REASON_PRESENTING);
            pgraph_wgpu_dl_reason = "display";
            pgraph_wgpu_surface_download_if_dirty(d, s);
        }
    }
    if (getenv("XEMU_VRAM_DISPLAY") || !bind_render_surface(d, r, &bg)) {
        if (!upload_scanout(d, r) || !disp->bind_group) {
            return;
        }
        bg = disp->bind_group;
    }
    if (!bg) {
        return; /* unchanged since the last present */
    }

    DisplayUniforms u = { 0 };
    memcpy(u.xform, disp->xform_values, sizeof(u.xform));
    u.display_size[0] = guest_display_w;
    u.display_size[1] = guest_display_h;
    WGPUTextureView pv_before = disp->pvideo_view;
    update_pvideo(d, r, &u);
    wgpuQueueWriteBuffer(r->queue, disp->xform, 0, &u, sizeof(u));
    if (disp->pvideo_view != pv_before) {
        /* overlay texture was resized: rebuild the bind group we use */
        if (bg == disp->bind_group) {
            wgpuBindGroupRelease(disp->bind_group);
            disp->bind_group = create_bind_group(r, disp->view);
            bg = disp->bind_group;
        } else {
            wgpuBindGroupRelease(disp->surface_bind_group);
            disp->surface_bind_group =
                create_bind_group(r, disp->surface_bound_view);
            bg = disp->surface_bind_group;
        }
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
    wgpuRenderPassEncoderSetBindGroup(pass, 0, bg, 0, NULL);
    wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
    wgpuRenderPassEncoderEnd(pass);
    WGPUCommandBuffer cb = wgpuCommandEncoderFinish(enc, NULL);
    wgpuQueueSubmit(r->queue, 1, &cb);

    wgpuCommandBufferRelease(cb);
    wgpuRenderPassEncoderRelease(pass);
    wgpuCommandEncoderRelease(enc);
    wgpuTextureViewRelease(target);
    wgpuTextureRelease(st.texture);

    /* A worker's OffscreenCanvas only presents when its task ends: the pfifo
     * thread must yield to the event loop for this frame to show. That yield
     * (~ms) happens in process_pending AFTER pgraph.lock is released, so guest
     * PGRAPH register accesses don't wait behind it. */
    disp->need_yield = true;

    extern volatile uint32_t xemu_wasm_present_count;
    xemu_wasm_present_count++;
    XSTAT_INC(n_present);
    XSTAT_T1(ns_present);

    static unsigned presented;
    if (++presented == 1) {
        fprintf(stderr, "[wgpu] presented %u frames (%dx%d)\n", presented,
                disp->width, disp->height);
    }
}
