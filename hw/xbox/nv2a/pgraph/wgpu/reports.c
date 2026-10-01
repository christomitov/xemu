/*
 * Geforce NV2A PGRAPH WebGPU Renderer: zpass pixel count reports
 *
 * Port of vk/reports.c. The Vulkan renderer keeps one occlusion query open
 * per NV2A "logical" query and lets it span several render passes. WebGPU
 * occlusion queries live inside a single render pass, so a logical query is
 * recorded as one or more WebGPU queries ("segments", begun lazily at the
 * first draw in each pass); gpu_query_logical[] maps each segment to its
 * logical query and the results are summed. Queries are resolved at the end
 * of each submit and read back after the queue went idle.
 *
 * Copyright (c) 2024 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#include "renderer.h"

void pgraph_wgpu_init_reports(PGRAPHState *pg)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;
    PGRAPHWgpuDrawState *ds = &r->draw;

    QSIMPLEQ_INIT(&ds->report_queue);
    ds->num_queries_in_flight = 0;
    ds->new_query_needed = false;
    ds->query_in_flight = false;
    ds->num_gpu_queries = 0;
    ds->gpu_query_active = false;
    ds->num_gpu_queries_resolved = 0;
    ds->zpass_pixel_count_result = 0;

    WGPUQuerySetDescriptor desc = {
        .label = { "nv2a-occlusion", WGPU_STRLEN },
        .type = WGPUQueryType_Occlusion,
        .count = WGPU_MAX_GPU_QUERIES,
    };
    ds->query_set = wgpuDeviceCreateQuerySet(r->device, &desc);
}

void pgraph_wgpu_finalize_reports(PGRAPHState *pg)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;
    PGRAPHWgpuDrawState *ds = &r->draw;

    /* submit anything still referencing the query set */
    if (ds->in_command_buffer) {
        pgraph_wgpu_finish(pg, WGPU_FINISH_REASON_FLUSH);
    }

    WgpuQueryReport *report;
    while ((report = QSIMPLEQ_FIRST(&ds->report_queue)) != NULL) {
        QSIMPLEQ_REMOVE_HEAD(&ds->report_queue, entry);
        g_free(report);
    }

    if (ds->query_set) {
        wgpuQuerySetDestroy(ds->query_set);
        wgpuQuerySetRelease(ds->query_set);
        ds->query_set = NULL;
    }
    ds->num_gpu_queries_resolved = 0;
}

/* Start a logical query (vk begin_query). The WebGPU query itself is begun
 * at the next draw inside a render pass. */
void pgraph_wgpu_begin_query(PGRAPHState *pg)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;
    PGRAPHWgpuDrawState *ds = &r->draw;

    assert(ds->in_command_buffer);
    assert(!ds->query_in_flight);
    assert(ds->num_queries_in_flight < WGPU_MAX_QUERIES_IN_FLIGHT);

    nv2a_profile_inc_counter(NV2A_PROF_QUERY);
    ds->query_in_flight = true;
    ds->new_query_needed = false;
    ds->num_queries_in_flight++;
}

void pgraph_wgpu_end_query(PGRAPHState *pg)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;
    PGRAPHWgpuDrawState *ds = &r->draw;

    assert(ds->query_in_flight);
    if (ds->gpu_query_active) {
        pgraph_wgpu_end_gpu_query(pg);
    }
    ds->query_in_flight = false;
}

void pgraph_wgpu_begin_gpu_query_if_needed(PGRAPHState *pg)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;
    PGRAPHWgpuDrawState *ds = &r->draw;

    if (!ds->query_in_flight || ds->gpu_query_active) {
        return;
    }
    assert(ds->in_render_pass);
    /* capacity is reserved in begin_pre_draw */
    assert(ds->num_gpu_queries < WGPU_MAX_GPU_QUERIES);

    ds->gpu_query_logical[ds->num_gpu_queries] =
        ds->num_queries_in_flight - 1;
    wgpuRenderPassEncoderBeginOcclusionQuery(ds->pass, ds->num_gpu_queries);
    ds->num_gpu_queries++;
    ds->gpu_query_active = true;
}

void pgraph_wgpu_end_gpu_query(PGRAPHState *pg)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;
    PGRAPHWgpuDrawState *ds = &r->draw;

    assert(ds->gpu_query_active && ds->in_render_pass);
    wgpuRenderPassEncoderEndOcclusionQuery(ds->pass);
    ds->gpu_query_active = false;
}

/* Record resolve + copy of this submit's queries (called by finish, outside
 * of any render pass). */
void pgraph_wgpu_resolve_queries(PGRAPHState *pg)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;
    PGRAPHWgpuDrawState *ds = &r->draw;

    assert(!ds->in_render_pass);
    assert(ds->num_gpu_queries_resolved == 0 || !ds->num_gpu_queries);

    if (!ds->num_gpu_queries) {
        return;
    }

    size_t size = ds->num_gpu_queries * sizeof(uint64_t);
    wgpuCommandEncoderResolveQuerySet(
        ds->encoder, ds->query_set, 0, ds->num_gpu_queries,
        ds->storage_buffers[WGPU_BUFFER_QUERY_RESOLVE].buffer, 0);
    wgpuCommandEncoderCopyBufferToBuffer(
        ds->encoder, ds->storage_buffers[WGPU_BUFFER_QUERY_RESOLVE].buffer, 0,
        ds->storage_buffers[WGPU_BUFFER_QUERY_READBACK].buffer, 0, size);
    ds->num_gpu_queries_resolved = ds->num_gpu_queries;
}

void pgraph_wgpu_clear_report_value(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;

    WgpuQueryReport *report = g_malloc(sizeof(WgpuQueryReport));
    report->clear = true;
    report->parameter = 0;
    report->query_count = r->draw.num_queries_in_flight;
    QSIMPLEQ_INSERT_TAIL(&r->draw.report_queue, report, entry);

    r->draw.new_query_needed = true;
}

void pgraph_wgpu_get_report(NV2AState *d, uint32_t parameter)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;

    uint8_t type = GET_MASK(parameter, NV097_GET_REPORT_TYPE);
    assert(type == NV097_GET_REPORT_TYPE_ZPASS_PIXEL_CNT);

    /*
     * Finish may write reports recursively during a surface download. Drain
     * retained ownership BEFORE enqueuing any DMA write, and disallow new
     * retention while reports are queued. Do not download from report finish.
     */
    pgraph_wgpu_materialize_retained(d, 0, memory_region_size(d->vram), false);

    WgpuQueryReport *report = g_malloc(sizeof(WgpuQueryReport));
    report->clear = false;
    report->parameter = parameter;
    report->query_count = r->draw.num_queries_in_flight;
    QSIMPLEQ_INSERT_TAIL(&r->draw.report_queue, report, entry);

    r->draw.new_query_needed = true;
}

void pgraph_wgpu_process_pending_reports_internal(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;
    PGRAPHWgpuDrawState *ds = &r->draw;

    assert(!ds->in_command_buffer);

    /* Fetch all query results, summed per logical query */
    g_autofree uint64_t *query_results = NULL;
    if (ds->num_queries_in_flight > 0) {
        query_results = g_malloc0_n(ds->num_queries_in_flight,
                                    sizeof(uint64_t));
    }
    if (ds->num_gpu_queries_resolved > 0) {
        g_autofree uint64_t *gpu_results =
            g_malloc_n(ds->num_gpu_queries_resolved, sizeof(uint64_t));
        pgraph_wgpu_read_buffer_sync(
            r, ds->storage_buffers[WGPU_BUFFER_QUERY_READBACK].buffer, 0,
            ds->num_gpu_queries_resolved * sizeof(uint64_t), gpu_results);
        for (int i = 0; i < ds->num_gpu_queries_resolved; i++) {
            int logical = ds->gpu_query_logical[i];
            if (query_results && logical < ds->num_queries_in_flight) {
                query_results[logical] += gpu_results[i];
            }
        }
        ds->num_gpu_queries_resolved = 0;
    }

    /* Write out queries */
    int num_results_counted = 0;
    const int result_divisor =
        pg->surface_scale_factor * pg->surface_scale_factor;

    WgpuQueryReport *report;
    while ((report = QSIMPLEQ_FIRST(&ds->report_queue)) != NULL) {
        assert(report->query_count >= num_results_counted);
        assert(report->query_count <= ds->num_queries_in_flight);

        while (num_results_counted < report->query_count) {
            ds->zpass_pixel_count_result +=
                query_results[num_results_counted++];
        }

        if (report->clear) {
            ds->zpass_pixel_count_result = 0;
        } else {
            pgraph_write_zpass_pixel_cnt_report(
                d, report->parameter,
                ds->zpass_pixel_count_result / result_divisor);
        }

        QSIMPLEQ_REMOVE_HEAD(&ds->report_queue, entry);
        g_free(report);
    }

    /* Add remaining results */
    while (num_results_counted < ds->num_queries_in_flight) {
        ds->zpass_pixel_count_result += query_results[num_results_counted++];
    }

    ds->num_queries_in_flight = 0;
}

void pgraph_wgpu_process_pending_reports(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;

    uint32_t *dma_get = &d->pfifo.regs[NV_PFIFO_CACHE1_DMA_GET];
    uint32_t *dma_put = &d->pfifo.regs[NV_PFIFO_CACHE1_DMA_PUT];

    if (*dma_get == *dma_put && r->draw.in_command_buffer &&
        !QSIMPLEQ_EMPTY(&r->draw.report_queue)) {
        pgraph_wgpu_finish(pg, WGPU_FINISH_REASON_STALLED);
    }
}
