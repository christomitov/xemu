/*
 * Geforce NV2A PGRAPH WebGPU Renderer (browser / emscripten)
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#ifndef HW_XBOX_NV2A_PGRAPH_WGPU_RENDERER_H
#define HW_XBOX_NV2A_PGRAPH_WGPU_RENDERER_H

#include "qemu/osdep.h"
#include "qemu/thread.h"
#include "hw/hw.h"
#include "hw/xbox/nv2a/nv2a_int.h"

#include <webgpu/webgpu.h>

typedef struct PGRAPHWgpuDisplayState {
    WGPURenderPipeline pipeline;
    WGPUBindGroupLayout bind_group_layout;
    WGPUSampler sampler;

    /* scanout source: VRAM copy of the current framebuffer */
    WGPUTexture texture;
    WGPUTextureView view;
    WGPUBindGroup bind_group;
    int width, height;
    uint32_t *conv; /* 15/16bpp -> RGBA8 staging */
    size_t conv_size;
} PGRAPHWgpuDisplayState;

typedef struct PGRAPHWgpuState {
    WGPUInstance instance;
    WGPUAdapter adapter;
    WGPUDevice device;
    WGPUQueue queue;

    WGPUSurface surface;
    WGPUTextureFormat surface_format;
    int surface_width, surface_height;

    PGRAPHWgpuDisplayState display;
} PGRAPHWgpuState;

/* renderer.c */
void pgraph_wgpu_wait(PGRAPHWgpuState *r, WGPUFuture future);
WGPUShaderModule pgraph_wgpu_create_wgsl_module(PGRAPHWgpuState *r,
                                               const char *label,
                                               const char *wgsl);

/* display.c */
void pgraph_wgpu_init_display(PGRAPHState *pg);
void pgraph_wgpu_finalize_display(PGRAPHState *pg);
void pgraph_wgpu_render_display(NV2AState *d);

#endif
