/*
 * Geforce NV2A PGRAPH WebGPU Renderer: vertex input
 *
 * Port of vk/vertex.c (+ the remap_unaligned_attributes path of vk/draw.c).
 *
 * WebGPU constraints handled here:
 *  - vertex buffer offsets / arrayStride must be multiples of 4, attribute
 *    offsets multiples of min(4, format size), offset + size <= arrayStride
 *  - no 3-component 8/16-bit formats, no scaled (S32K) formats, and only
 *    limits.maxVertexBuffers (usually 8) vertex buffer slots for up to 16
 *    NV2A attributes.
 * Attributes that share a source buffer and stride and fit in one stride
 * window are bound through a single slot (typical interleaved vertices).
 * Anything that cannot be expressed natively is converted on the CPU to
 * float32xN (CMP stays packed as sint32 for the shader to decompress) and
 * interleaved into one "repack" slot in the inline vertex buffer.
 *
 * All draws are indexed with indices relative to min_element (see draw.c),
 * so every slot is bound at the address of element min_element.
 *
 * Copyright (c) 2024 Matt Borgerson
 *
 * Based on GL implementation:
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2015 Jannik Vogel
 * Copyright (c) 2018-2024 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#include "renderer.h"

size_t pgraph_wgpu_update_index_buffer(PGRAPHState *pg, void *data,
                                       size_t size)
{
    nv2a_profile_inc_counter(NV2A_PROF_GEOM_BUFFER_UPDATE_2);
    return pgraph_wgpu_append_to_buffer(pg, WGPU_BUFFER_INDEX, &data, &size,
                                        1, 4);
}

size_t pgraph_wgpu_update_vertex_inline_buffer(PGRAPHState *pg, void **data,
                                               size_t *sizes, size_t count)
{
    nv2a_profile_inc_counter(NV2A_PROF_GEOM_BUFFER_UPDATE_3);
    return pgraph_wgpu_append_to_buffer(pg, WGPU_BUFFER_VERTEX_INLINE, data,
                                        sizes, count, 4);
}

void pgraph_wgpu_update_vertex_ram_buffer(PGRAPHState *pg, hwaddr offset,
                                          void *data, size_t size)
{
    NV2AState *d = container_of(pg, NV2AState, pgraph);
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;
    PGRAPHWgpuDrawState *ds = &r->draw;
    WgpuStorageBuffer *b = &ds->storage_buffers[WGPU_BUFFER_VERTEX_RAM];

    if (!size) {
        return;
    }

    pgraph_wgpu_download_surfaces_in_range_if_dirty(pg, offset, size);

    size_t start_bit = offset / TARGET_PAGE_SIZE;
    size_t end_bit = TARGET_PAGE_ALIGN(offset + size) / TARGET_PAGE_SIZE;
    size_t nbits = end_bit - start_bit;

    if (find_next_bit(ds->uploaded_bitmap, start_bit + nbits, start_bit) <
        end_bit) {
        // Vertex data changed while building the draw list. Finish drawing
        // before updating RAM buffer.
        pgraph_wgpu_finish(pg, WGPU_FINISH_REASON_VERTEX_BUFFER_DIRTY);
    }

    nv2a_profile_inc_counter(NV2A_PROF_GEOM_BUFFER_UPDATE_1);

    /* wgpuQueueWriteBuffer needs 4-byte alignment; the buffer mirrors VRAM,
     * so widen the range and source the extra bytes from VRAM */
    hwaddr aligned_start = offset & ~(hwaddr)3;
    hwaddr aligned_end = ROUND_UP(offset + size, 4);
    hwaddr vram_size = memory_region_size(d->vram);
    if (aligned_end > b->buffer_size) {
        aligned_end = b->buffer_size;
    }
    if (aligned_start >= aligned_end) {
        return;
    }
    if (aligned_start == offset && aligned_end == offset + size) {
        wgpuQueueWriteBuffer(r->queue, b->buffer, offset, data, size);
    } else if (data == d->vram_ptr + offset && aligned_end <= vram_size) {
        wgpuQueueWriteBuffer(r->queue, b->buffer, aligned_start,
                             d->vram_ptr + aligned_start,
                             aligned_end - aligned_start);
    } else {
        size_t len = aligned_end - aligned_start;
        g_autofree uint8_t *tmp = g_malloc0(len);
        size_t lead = offset - aligned_start;
        if (aligned_start < vram_size) {
            memcpy(tmp, d->vram_ptr + aligned_start,
                   MIN(len, vram_size - aligned_start));
        }
        memcpy(tmp + lead, data, MIN(size, len - lead));
        wgpuQueueWriteBuffer(r->queue, b->buffer, aligned_start, tmp, len);
    }

    bitmap_set(ds->uploaded_bitmap, start_bit, nbits);
}

static void update_memory_buffer(NV2AState *d, hwaddr addr, hwaddr size)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;
    PGRAPHWgpuDrawState *ds = &r->draw;

    assert(ds->num_vertex_ram_buffer_syncs <
           ARRAY_SIZE(ds->vertex_ram_buffer_syncs));
    ds->vertex_ram_buffer_syncs[ds->num_vertex_ram_buffer_syncs++] =
        (WgpuMemorySyncRequirement){ .addr = addr, .size = size };
}

static const WGPUVertexFormat float_to_count[] = {
    WGPUVertexFormat_Float32,
    WGPUVertexFormat_Float32x2,
    WGPUVertexFormat_Float32x3,
    WGPUVertexFormat_Float32x4,
};

/* Native WebGPU format for an NV2A array attribute, or 0 if the data must
 * be converted on the CPU */
static WGPUVertexFormat get_native_format(const VertexAttribute *attr)
{
    switch (attr->format) {
    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_UB_D3D:
    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_UB_OGL:
        if (attr->count == 2) {
            return WGPUVertexFormat_Unorm8x2;
        } else if (attr->count == 4) {
            return WGPUVertexFormat_Unorm8x4;
        }
        return 0;
    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_S1:
        if (attr->count == 2) {
            return WGPUVertexFormat_Snorm16x2;
        } else if (attr->count == 4) {
            return WGPUVertexFormat_Snorm16x4;
        }
        return 0;
    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F:
        return float_to_count[attr->count - 1];
    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_S32K:
        /* SSCALED: no WebGPU equivalent that yields floats */
        return 0;
    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_CMP:
        /* 3 signed, normalized components packed in 32-bits (11,11,10),
         * decompressed by the vertex shader (compressed_attrs) */
        return WGPUVertexFormat_Sint32;
    default:
        return 0;
    }
}

static WGPUVertexFormat get_repack_format(const VertexAttribute *attr,
                                          uint32_t *size)
{
    if (attr->format == NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_CMP) {
        *size = 4;
        return WGPUVertexFormat_Sint32;
    }
    *size = 4 * attr->count;
    return float_to_count[attr->count - 1];
}

/* Convert one element to its repack format (see get_repack_format) */
static void repack_element(const VertexAttribute *attr, const uint8_t *src,
                           uint8_t *out)
{
    float *f = (float *)out;

    switch (attr->format) {
    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_UB_D3D:
    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_UB_OGL:
        /* UNORM8 */
        for (int c = 0; c < attr->count; c++) {
            f[c] = src[c] / 255.0f;
        }
        break;
    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_S1:
        /* SNORM16 */
        for (int c = 0; c < attr->count; c++) {
            int16_t v;
            memcpy(&v, src + 2 * c, 2);
            f[c] = MAX(v / 32767.0f, -1.0f);
        }
        break;
    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_S32K:
        /* SSCALED16 */
        for (int c = 0; c < attr->count; c++) {
            int16_t v;
            memcpy(&v, src + 2 * c, 2);
            f[c] = v;
        }
        break;
    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F:
        memcpy(out, src, 4 * attr->count);
        break;
    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_CMP:
        memcpy(out, src, 4);
        break;
    default:
        memset(out, 0, 4 * attr->count);
        break;
    }
}

typedef struct VertexAttrCandidate {
    int attr;
    WgpuVertexSource source;
    hwaddr addr;    /* element 0, relative to the source */
    uint32_t stride;
    uint32_t size;  /* bytes per element */
    WGPUVertexFormat native; /* 0 if not usable natively */
    const uint8_t *src; /* CPU pointer to element 0 */
    int group;      /* -1: repack */
} VertexAttrCandidate;

typedef struct VertexGroup {
    WgpuVertexSource source;
    uint32_t stride;
    hwaddr lo, hi;
} VertexGroup;

static void reset_vertex_state(PGRAPHWgpuDrawState *ds)
{
    memset(&ds->vertex_layout, 0, sizeof(ds->vertex_layout));
    memset(&ds->vertex_buffers, 0, sizeof(ds->vertex_buffers));
    memset(&ds->repack, 0, sizeof(ds->repack));
    ds->repack.slot = -1;
    ds->inline_array_mode = false;
}

static void build_vertex_layout(PGRAPHState *pg, VertexAttrCandidate *cands,
                                int num_cands)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;
    PGRAPHWgpuDrawState *ds = &r->draw;

    VertexGroup groups[NV2A_VERTEXSHADER_ATTRIBUTES];
    int num_groups = 0;
    bool any_repack = false;

    /* Group natively usable attributes by (source, stride, window) */
    for (int i = 0; i < num_cands; i++) {
        VertexAttrCandidate *c = &cands[i];
        c->group = -1;

        bool native = c->native && (c->stride % 4 == 0) &&
                      (c->addr % MIN(4, c->size) == 0) &&
                      c->size <= c->stride;
        if (!native) {
            any_repack = true;
            continue;
        }

        hwaddr lo = c->addr & ~(hwaddr)3, hi = c->addr + c->size;
        for (int g = 0; g < num_groups; g++) {
            VertexGroup *grp = &groups[g];
            if (grp->source != c->source || grp->stride != c->stride) {
                continue;
            }
            hwaddr nlo = MIN(grp->lo, lo), nhi = MAX(grp->hi, hi);
            if (nhi - nlo <= c->stride) {
                grp->lo = nlo;
                grp->hi = nhi;
                c->group = g;
                break;
            }
        }
        if (c->group < 0) {
            groups[num_groups] = (VertexGroup){
                .source = c->source, .stride = c->stride, .lo = lo, .hi = hi,
            };
            c->group = num_groups++;
        }
    }

    /* Respect maxVertexBuffers: fold trailing groups into the repack slot */
    int max_slots = MIN(r->limits.maxVertexBuffers,
                        NV2A_VERTEXSHADER_ATTRIBUTES);
    while (num_groups > 0 && num_groups + (any_repack ? 1 : 0) > max_slots) {
        num_groups--;
        for (int i = 0; i < num_cands; i++) {
            if (cands[i].group == num_groups) {
                cands[i].group = -1;
            }
        }
        any_repack = true;
    }
    /* Groups may have shrunk; recompute windows of the remaining ones */
    for (int g = 0; g < num_groups; g++) {
        groups[g].lo = UINT64_MAX;
        groups[g].hi = 0;
    }
    for (int i = 0; i < num_cands; i++) {
        VertexAttrCandidate *c = &cands[i];
        if (c->group >= 0) {
            VertexGroup *grp = &groups[c->group];
            grp->lo = MIN(grp->lo, c->addr & ~(hwaddr)3);
            grp->hi = MAX(grp->hi, c->addr + c->size);
        }
    }

    WgpuVertexLayoutKey *layout = &ds->vertex_layout;
    for (int g = 0; g < num_groups; g++) {
        layout->strides[g] = groups[g].stride;
        ds->vertex_buffers[g] = (WgpuVertexBufferBinding){
            .source = groups[g].source,
            .base = groups[g].lo + (hwaddr)ds->min_element * groups[g].stride,
        };
    }
    layout->num_buffers = num_groups;

    WgpuVertexRepack *rp = &ds->repack;
    if (any_repack) {
        rp->slot = num_groups;
        ds->vertex_buffers[rp->slot] = (WgpuVertexBufferBinding){
            .source = WGPU_VSRC_REPACK,
        };
        layout->num_buffers++;
    }

    for (int i = 0; i < num_cands; i++) {
        VertexAttrCandidate *c = &cands[i];
        WgpuVertexAttrKey *k = &layout->attrs[layout->num_attrs++];
        k->location = c->attr;
        if (c->group >= 0) {
            k->buffer = c->group;
            k->format = c->native;
            k->offset = c->addr - groups[c->group].lo;
        } else {
            uint32_t out_size;
            k->buffer = rp->slot;
            k->format =
                get_repack_format(&pg->vertex_attributes[c->attr], &out_size);
            k->offset = rp->stride;
            rp->attrs |= 1 << c->attr;
            rp->out_offset[c->attr] = rp->stride;
            rp->src[c->attr] = c->src;
            rp->src_stride[c->attr] = c->stride;
            rp->stride += out_size;
        }
    }
    if (any_repack) {
        layout->strides[rp->slot] = rp->stride;
    }
}

void pgraph_wgpu_bind_vertex_attributes(NV2AState *d, unsigned int min_element,
                                        unsigned int max_element,
                                        bool inline_data,
                                        unsigned int inline_stride,
                                        unsigned int provoking_element)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;
    PGRAPHWgpuDrawState *ds = &r->draw;

    unsigned int num_elements = max_element - min_element + 1;

    NV2A_DPRINTF("%s (num_elements: %d inline: %d stride: %d)\n", __func__,
                 num_elements, inline_data, inline_stride);

    pg->compressed_attrs = 0;
    pg->uniform_attrs = 0;
    pg->swizzle_attrs = 0;

    reset_vertex_state(ds);
    ds->min_element = min_element;
    ds->num_elements = num_elements;
    ds->inline_array_mode = inline_data;
    ds->repack.src_limit =
        inline_data ?
            (const uint8_t *)pg->inline_array + pg->inline_array_length * 4 :
            d->vram_ptr + memory_region_size(d->vram);

    VertexAttrCandidate cands[NV2A_VERTEXSHADER_ATTRIBUTES];
    int num_cands = 0;

    for (int i = 0; i < NV2A_VERTEXSHADER_ATTRIBUTES; i++) {
        VertexAttribute *attr = &pg->vertex_attributes[i];
        NV2A_DPRINTF("[attr %02d] format=%d, count=%d, stride=%d\n", i,
                     attr->format, attr->count, attr->stride);
        if (!attr->count) {
            pg->uniform_attrs |= 1 << i;
            continue;
        }

        bool needs_conversion = false;
        bool d3d_swizzle = false;

        switch (attr->format) {
        case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_UB_D3D:
            assert(attr->count == 4);
            d3d_swizzle = true;
            /* fallthru */
        case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_UB_OGL:
        case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_S1:
        case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F:
        case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_S32K:
            assert(attr->count <= 4);
            break;
        case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_CMP:
            assert(attr->count == 1);
            needs_conversion = true;
            break;
        default:
            fprintf(stderr, "Unknown vertex type: 0x%x\n", attr->format);
            assert(!"Unknown vertex type");
            break;
        }

        nv2a_profile_inc_counter(NV2A_PROF_ATTR_BIND);
        hwaddr attrib_data_addr;
        size_t stride;
        const uint8_t *src_base;

        hwaddr start = 0;
        if (inline_data) {
            attrib_data_addr = attr->inline_array_offset;
            stride = inline_stride;
            src_base = (const uint8_t *)pg->inline_array;
        } else {
            hwaddr dma_len;
            uint8_t *attr_data = (uint8_t *)nv_dma_map(
                d, attr->dma_select ? pg->dma_vertex_b : pg->dma_vertex_a,
                &dma_len);
            assert(attr->offset < dma_len);
            attrib_data_addr = attr_data + attr->offset - d->vram_ptr;
            stride = attr->stride;
            start = attrib_data_addr + min_element * stride;
            update_memory_buffer(d, start, num_elements * stride);
            src_base = d->vram_ptr;
        }

        uint32_t provoking_element_index = provoking_element - min_element;
        size_t element_size = attr->size * attr->count;
        assert(element_size <= sizeof(attr->inline_value));
        const uint8_t *last_entry;

        if (inline_data) {
            last_entry =
                (uint8_t *)pg->inline_array + attr->inline_array_offset;
        } else {
            last_entry = d->vram_ptr + start;
        }
        if (!stride) {
            // Stride of 0 indicates that only the first element should be
            // used.
            pg->uniform_attrs |= 1 << i;
            pgraph_update_inline_value(attr, last_entry);
            continue;
        }

        last_entry += stride * provoking_element_index;
        pgraph_update_inline_value(attr, last_entry);

        cands[num_cands++] = (VertexAttrCandidate){
            .attr = i,
            .source = inline_data ? WGPU_VSRC_INLINE_ARRAY : WGPU_VSRC_VRAM,
            .addr = attrib_data_addr,
            .stride = stride,
            .size = element_size,
            .native = get_native_format(attr),
            .src = src_base + attrib_data_addr,
        };

        if (needs_conversion) {
            pg->compressed_attrs |= (1 << i);
        }
        if (d3d_swizzle) {
            pg->swizzle_attrs |= (1 << i);
        }
    }

    build_vertex_layout(pg, cands, num_cands);
}

void pgraph_wgpu_bind_vertex_attributes_inline(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;
    PGRAPHWgpuDrawState *ds = &r->draw;
    WgpuVertexLayoutKey *layout = &ds->vertex_layout;
    WgpuVertexRepack *rp = &ds->repack;

    pg->compressed_attrs = 0;
    pg->uniform_attrs = 0;
    pg->swizzle_attrs = 0;

    reset_vertex_state(ds);
    ds->min_element = 0;
    ds->num_elements = pg->inline_buffer_length;

    /* All populated attributes are interleaved (float4 each) into one slot */
    for (int i = 0; i < NV2A_VERTEXSHADER_ATTRIBUTES; i++) {
        VertexAttribute *attr = &pg->vertex_attributes[i];
        if (attr->inline_buffer_populated) {
            WgpuVertexAttrKey *k = &layout->attrs[layout->num_attrs++];
            k->location = i;
            k->buffer = 0;
            k->format = WGPUVertexFormat_Float32x4;
            k->offset = rp->stride;
            rp->attrs |= 1 << i;
            rp->out_offset[i] = rp->stride;
            rp->stride += 4 * sizeof(float);
            memcpy(attr->inline_value,
                   attr->inline_buffer + (pg->inline_buffer_length - 1) * 4,
                   sizeof(attr->inline_value));
        } else {
            pg->uniform_attrs |= 1 << i;
        }
    }

    if (rp->attrs) {
        rp->inline_buffer = true;
        rp->slot = 0;
        layout->num_buffers = 1;
        layout->strides[0] = rp->stride;
        ds->vertex_buffers[0] = (WgpuVertexBufferBinding){
            .source = WGPU_VSRC_REPACK,
        };
    }
}

/* Bytes that pgraph_wgpu_upload_vertex_data() will append to the inline
 * vertex buffer (including alignment slack) */
size_t pgraph_wgpu_vertex_upload_size(PGRAPHState *pg)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;
    PGRAPHWgpuDrawState *ds = &r->draw;
    size_t size = 0;

    if (ds->inline_array_mode) {
        size += ROUND_UP(pg->inline_array_length * 4, 4) + 4;
    }
    if (ds->repack.slot >= 0) {
        size += (size_t)ds->repack.stride * ds->num_elements + 4;
    }
    return size;
}

static uint8_t *get_scratch(PGRAPHWgpuDrawState *ds, size_t size)
{
    if (ds->scratch_size < size) {
        ds->scratch = g_realloc(ds->scratch, size);
        ds->scratch_size = size;
    }
    return ds->scratch;
}

void pgraph_wgpu_upload_vertex_data(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;
    PGRAPHWgpuDrawState *ds = &r->draw;
    WgpuVertexRepack *rp = &ds->repack;

    if (ds->inline_array_mode) {
        void *data = pg->inline_array;
        size_t size = pg->inline_array_length * 4;
        ds->inline_array_offset =
            pgraph_wgpu_update_vertex_inline_buffer(pg, &data, &size, 1);
    }

    if (rp->slot < 0) {
        return;
    }

    size_t size = (size_t)rp->stride * ds->num_elements;
    uint8_t *out = get_scratch(ds, MAX(size, 4));

    if (rp->inline_buffer) {
        for (unsigned int v = 0; v < ds->num_elements; v++) {
            uint8_t *o = out + (size_t)v * rp->stride;
            for (int i = 0; i < NV2A_VERTEXSHADER_ATTRIBUTES; i++) {
                if (rp->attrs & (1 << i)) {
                    memcpy(o + rp->out_offset[i],
                           pg->vertex_attributes[i].inline_buffer + v * 4,
                           4 * sizeof(float));
                }
            }
        }
    } else {
        for (int i = 0; i < NV2A_VERTEXSHADER_ATTRIBUTES; i++) {
            if (!(rp->attrs & (1 << i))) {
                continue;
            }
            VertexAttribute *attr = &pg->vertex_attributes[i];
            uint32_t out_size;
            get_repack_format(attr, &out_size);
            size_t in_size = attr->size * attr->count;
            const uint8_t *in =
                rp->src[i] + (size_t)ds->min_element * rp->src_stride[i];
            uint8_t *o = out + rp->out_offset[i];
            for (unsigned int v = 0; v < ds->num_elements; v++) {
                if ((uintptr_t)in + in_size > (uintptr_t)rp->src_limit) {
                    memset(o, 0, out_size);
                } else {
                    repack_element(attr, in, o);
                }
                o += rp->stride;
                in += rp->src_stride[i];
            }
        }
    }

    void *data = out;
    ds->repack_offset =
        pgraph_wgpu_update_vertex_inline_buffer(pg, &data, &size, 1);
}

/* Bind all vertex buffer slots on the current render pass. Returns false if
 * a binding is out of range (draw must be skipped). */
bool pgraph_wgpu_set_vertex_buffers(PGRAPHState *pg)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;
    PGRAPHWgpuDrawState *ds = &r->draw;

    for (uint32_t slot = 0; slot < ds->vertex_layout.num_buffers; slot++) {
        WgpuVertexBufferBinding *b = &ds->vertex_buffers[slot];
        WgpuStorageBuffer *sb;
        uint64_t offset;

        switch (b->source) {
        case WGPU_VSRC_VRAM:
            sb = &ds->storage_buffers[WGPU_BUFFER_VERTEX_RAM];
            offset = b->base;
            break;
        case WGPU_VSRC_INLINE_ARRAY:
            sb = &ds->storage_buffers[WGPU_BUFFER_VERTEX_INLINE];
            offset = ds->inline_array_offset + b->base;
            break;
        case WGPU_VSRC_REPACK:
        default:
            sb = &ds->storage_buffers[WGPU_BUFFER_VERTEX_INLINE];
            offset = ds->repack_offset;
            break;
        }

        assert(offset % 4 == 0);
        if (offset >= sb->buffer_size) {
            static bool logged;
            if (!logged) {
                fprintf(stderr, "[wgpu] vertex buffer slot %u offset 0x%" PRIx64
                        " out of range, skipping draw\n", slot, offset);
                logged = true;
            }
            return false;
        }
        wgpuRenderPassEncoderSetVertexBuffer(ds->pass, slot, sb->buffer, offset,
                                             WGPU_WHOLE_SIZE);
    }
    return true;
}
