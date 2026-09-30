/*
 * Geforce NV2A PGRAPH WebGPU Renderer: buffers
 *
 * Port of vk/buffer.c. WebGPU has no persistent mapping: all uploads use
 * wgpuQueueWriteBuffer, which lands on the queue timeline before the next
 * submit. Because of that, a region must not be rewritten while a draw
 * recorded earlier in the same (still unsubmitted) encoder needs the old
 * contents, so the index/inline vertex buffers are append-only between
 * finishes (offsets reset in pgraph_wgpu_finish), and the VRAM mirror uses
 * the uploaded_bitmap logic of the Vulkan renderer.
 *
 * Copyright (c) 2024 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#include "renderer.h"

static void create_buffer(PGRAPHWgpuState *r, WgpuStorageBuffer *b)
{
    WGPUBufferDescriptor desc = {
        .label = { b->label, WGPU_STRLEN },
        .usage = b->usage,
        .size = b->buffer_size,
    };
    b->buffer = wgpuDeviceCreateBuffer(r->device, &desc);
    b->buffer_offset = 0;
    if (!b->buffer) {
        fprintf(stderr, "[wgpu] failed to create buffer %s (%zu bytes)\n",
                b->label, b->buffer_size);
    }
}

static void destroy_buffer(WgpuStorageBuffer *b)
{
    g_free(b->staging);
    b->staging = NULL;
    if (b->buffer) {
        wgpuBufferDestroy(b->buffer);
        wgpuBufferRelease(b->buffer);
        b->buffer = NULL;
    }
}

void pgraph_wgpu_init_buffers(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;
    WgpuStorageBuffer *sb = r->draw.storage_buffers;

    /* FIXME: Profile buffer sizes. The Vulkan renderer's sizes (hundreds of
     * MiB) are too large for a browser; draws that do not fit are dropped
     * (see ensure_buffer_space in draw.c). */
    sb[WGPU_BUFFER_INDEX] = (WgpuStorageBuffer){
        .label = "nv2a-index",
        .usage = WGPUBufferUsage_Index | WGPUBufferUsage_CopyDst,
        .buffer_size = 16 * 1024 * 1024,
    };
    sb[WGPU_BUFFER_VERTEX_RAM] = (WgpuStorageBuffer){
        .label = "nv2a-vertex-ram",
        .usage = WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst,
        .buffer_size = ROUND_UP(memory_region_size(d->vram), 4),
    };
    sb[WGPU_BUFFER_VERTEX_INLINE] = (WgpuStorageBuffer){
        .label = "nv2a-vertex-inline",
        .usage = WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst,
        .buffer_size = 32 * 1024 * 1024,
    };
    sb[WGPU_BUFFER_QUERY_RESOLVE] = (WgpuStorageBuffer){
        .label = "nv2a-query-resolve",
        .usage = WGPUBufferUsage_QueryResolve | WGPUBufferUsage_CopySrc,
        .buffer_size = WGPU_MAX_GPU_QUERIES * sizeof(uint64_t),
    };
    sb[WGPU_BUFFER_QUERY_READBACK] = (WgpuStorageBuffer){
        .label = "nv2a-query-readback",
        .usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst,
        .buffer_size = WGPU_MAX_GPU_QUERIES * sizeof(uint64_t),
    };

    for (int i = 0; i < WGPU_BUFFER_COUNT; i++) {
        if (!sb[i].buffer_size) {
            continue;
        }
        if (sb[i].buffer_size > r->limits.maxBufferSize) {
            fprintf(stderr, "[wgpu] buffer %s: %zu bytes exceeds "
                    "maxBufferSize %" PRIu64 "\n", sb[i].label,
                    sb[i].buffer_size, (uint64_t)r->limits.maxBufferSize);
        }
        create_buffer(r, &sb[i]);
    }

    r->draw.bitmap_size = memory_region_size(d->vram) / 4096;
    r->draw.uploaded_bitmap = bitmap_new(r->draw.bitmap_size);
    bitmap_clear(r->draw.uploaded_bitmap, 0, r->draw.bitmap_size);
}

void pgraph_wgpu_finalize_buffers(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;

    for (int i = 0; i < WGPU_BUFFER_COUNT; i++) {
        destroy_buffer(&r->draw.storage_buffers[i]);
    }

    g_free(r->draw.uploaded_bitmap);
    r->draw.uploaded_bitmap = NULL;
    g_free(r->draw.scratch);
    r->draw.scratch = NULL;
    r->draw.scratch_size = 0;
    g_free(r->draw.indices);
    r->draw.indices = NULL;
    r->draw.indices_capacity = 0;
}

bool pgraph_wgpu_buffer_has_space_for(PGRAPHState *pg, int index, size_t size,
                                      size_t alignment)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;
    WgpuStorageBuffer *b = &r->draw.storage_buffers[index];
    return (ROUND_UP(b->buffer_offset, alignment) + ROUND_UP(size, 4)) <=
           b->buffer_size;
}

/* wgpuQueueWriteBuffer requires 4-byte aligned offset and size */
static void write_buffer_padded(PGRAPHWgpuState *r, WGPUBuffer buffer,
                                size_t offset, const void *data, size_t size)
{
    size_t body = size & ~(size_t)3;
    if (body) {
        wgpuQueueWriteBuffer(r->queue, buffer, offset, data, body);
    }
    if (size != body) {
        uint8_t tail[4] = { 0 };
        memcpy(tail, (const uint8_t *)data + body, size - body);
        wgpuQueueWriteBuffer(r->queue, buffer, offset + body, tail, 4);
    }
}

size_t pgraph_wgpu_append_to_buffer(PGRAPHState *pg, int index, void **data,
                                    size_t *sizes, size_t count,
                                    size_t alignment)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;
    WgpuStorageBuffer *b = &r->draw.storage_buffers[index];

    alignment = MAX(alignment, 4);
    assert(alignment % 4 == 0);

    size_t total_size = 0;
    for (size_t i = 0; i < count; i++) {
        total_size += ROUND_UP(sizes[i], alignment);
    }
    assert(pgraph_wgpu_buffer_has_space_for(pg, index, total_size, alignment));

    size_t starting_offset = ROUND_UP(b->buffer_offset, alignment);

    bool staged = index == WGPU_BUFFER_INDEX ||
                  index == WGPU_BUFFER_VERTEX_INLINE;
    if (staged && !b->staging) {
        b->staging = g_malloc(b->buffer_size);
        b->staged_lo = b->staged_hi = 0;
    }

    for (size_t i = 0; i < count; i++) {
        b->buffer_offset = ROUND_UP(b->buffer_offset, alignment);
        if (sizes[i]) {
            if (staged) {
                size_t len = ROUND_UP(sizes[i], 4);
                memcpy(b->staging + b->buffer_offset, data[i], sizes[i]);
                memset(b->staging + b->buffer_offset + sizes[i], 0,
                       len - sizes[i]);
                if (b->staged_hi == b->staged_lo) {
                    b->staged_lo = b->buffer_offset;
                }
                b->staged_lo = MIN(b->staged_lo, b->buffer_offset);
                b->staged_hi = MAX(b->staged_hi, b->buffer_offset + len);
            } else {
                write_buffer_padded(r, b->buffer, b->buffer_offset, data[i],
                                    sizes[i]);
            }
        }
        b->buffer_offset += ROUND_UP(sizes[i], 4);
    }

    return starting_offset;
}

/* Upload staged appends (see WgpuStorageBuffer.staging); before a submit. */
void pgraph_wgpu_flush_staged(PGRAPHState *pg)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;
    static const int idx[] = { WGPU_BUFFER_INDEX, WGPU_BUFFER_VERTEX_INLINE };

    for (int i = 0; i < ARRAY_SIZE(idx); i++) {
        WgpuStorageBuffer *b = &r->draw.storage_buffers[idx[i]];
        if (b->staging && b->staged_hi > b->staged_lo) {
            wgpuQueueWriteBuffer(r->queue, b->buffer, b->staged_lo,
                                 b->staging + b->staged_lo,
                                 b->staged_hi - b->staged_lo);
        }
        b->staged_lo = b->staged_hi = 0;
    }
}
