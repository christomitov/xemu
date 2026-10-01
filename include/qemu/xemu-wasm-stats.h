/*
 * xemu-wasm performance counters, read by the host page's Stats panel.
 *
 * One global struct of plain 64-bit counters. Writers are the emulator
 * threads (each field has a single writer thread, or is only approximately
 * counted), the reader is the browser main thread via
 * xemu_wasm_stats_ptr(); torn reads just blur one sample.
 * Counters (n_*) are events, timers (ns_*) are accumulated nanoseconds;
 * the page turns both into per-second rates.
 *
 * Timers only wrap coarse events (lock waits, GPU waits, shader compiles):
 * clock reads under emscripten pthreads are not free.
 */
#ifndef QEMU_XEMU_WASM_STATS_H
#define QEMU_XEMU_WASM_STATS_H

#include <stdint.h>

#define XEMU_WASM_STATS_FIELDS(X)                                             \
    /* vCPU (TCG) */                                                         \
    X(n_tb_exec)         /* translated blocks executed */                    \
    X(n_tb_gen)          /* blocks translated */                             \
    X(n_cpu_exit)        /* longjmp exits from the cpu loop */               \
    X(n_helper)          /* helper calls from translated code (TCI) */       \
    X(n_tlb_fill)        /* softmmu TLB refills (page walks) */              \
    X(n_jit_compile)     /* TBs compiled to wasm modules */                  \
    X(ns_jit_compile)                                                        \
    X(n_jit_threshold)   /* current adaptive JIT threshold (gauge) */        \
    X(n_jit_instances)   /* live wasm modules (gauge) */                     \
    X(n_jit_evict)       /* modules dropped at the instance cap */           \
    X(n_jit_relink)      /* modules re-instantiated with fresh links */      \
    X(n_tb_linked)       /* TB transitions via a successor link */           \
    X(n_tb_selfloop)     /* goto_tb to the same TB (wasm loop) */            \
    X(n_tb_gotoptr)      /* indirect: goto_ptr after lookup_tb_ptr */        \
    X(n_tb_linkmiss)     /* chained goto_tb whose link was missing/stale */  \
    X(n_guest_insn)      /* guest insns in dispatched JIT TBs (not loops) */  \
    X(n_tb_invalidate)   /* TBs invalidated (SMC, page writes) */            \
    X(n_bind_group_create) /* WebGPU bind groups created (cache misses) */  \
    X(n_slow_st)         /* stores taking the softmmu slow path */          \
    X(n_slow_st_miss)    /*  ... TLB miss (victim hit or fill) */          \
    X(n_slow_st_notdirty) /* ... clean page (code / dirty tracking) */     \
    X(n_smc_bitmap_miss) /* ... code-page stores cleared by the code bitmap */ \
    X(n_slow_st_mmio)    /*  ... MMIO */                                    \
    X(n_slow_st_watch)   /*  ... watchpoint / access callback */            \
    X(n_slow_ld)         /* loads taking the softmmu slow path */           \
    X(n_slow_ld_miss)                                                        \
    X(n_slow_ld_mmio)                                                        \
    X(n_slow_ld_watch)                                                       \
    X(n_tlb_alias)       /* misses where the slot held the same low page bits */ \
    X(n_tb_recycle)      /* tb_gen served by reviving an invalidated TB */   \
    X(n_sse_checked)     /* XEMU_WASM_SSE_FAST=2: fast results verified */   \
    X(n_sse_mismatch)    /*  ... differing from the helper (must be 0) */    \
    X(n_jc_flush)        /* jump cache flushes (full TLB flushes) */        \
    X(n_tlb_reset_dirty) /* tlb_reset_dirty() scans of every TLB entry */   \
    X(n_tci_exec)        /* TBs run in the interpreter (not yet compiled) */ \
    X(n_tci_dropped)     /*  ... because their JIT instance was dropped */   \
    X(n_tb_flush)        /* whole translation cache flushed (buffer full) */ \
    X(n_test_unwind)     /* XEMU_WASM_TEST_UNWIND forced sleeps */            \
    X(n_jit_rewind)      /* Asyncify rewinds into JIT code (rewind fns) */   \
    X(n_region_compile)  /* region modules built */                          \
    X(n_region_members)  /* ... total member TBs in them */                  \
    X(n_tb_region)       /* TB transitions branched inside a region */       \
    X(n_jc_lookup)       /* tb_lookup calls (jump cache probes) */           \
    X(n_jc_miss)         /* ... that missed and went to the QHT */          \
    X(n_mmio_read)       /* guest loads from device registers */             \
    X(n_mmio_write)                                                          \
    X(ns_mmio)           /* time in device read/write handlers */            \
    X(n_watch_access)    /* CPU accesses trapped on GPU-surface pages */     \
    X(ns_vcpu_surface_wait) /* vCPU blocked on GPU surface readback */       \
    X(n_watch_disarm)    /* surface traps disarmed after first access */     \
    X(n_vcpu_bql_wait)   /* contended BQL acquisitions by the vCPU */        \
    X(ns_vcpu_bql_wait)                                                      \
    /* main loop */                                                          \
    X(n_main_iter)       /* main_loop_wait iterations */                     \
    X(n_main_bql_wait)   /* contended BQL acquisitions by other threads */   \
    X(ns_main_bql_wait)                                                      \
    /* NV2A / WebGPU renderer (pfifo thread) */                              \
    X(n_pgraph_method)   /* PGRAPH methods processed */                      \
    X(n_draw)            /* draws issued */                                  \
    X(n_clear)                                                               \
    X(n_pipeline_gen)    /* render pipelines created */                      \
    X(n_shader_gen)      /* shaders generated + translated */                \
    X(ns_shader_gen)                                                         \
    X(n_submit)          /* queue submits (pgraph_wgpu_finish) */            \
    X(ns_gpu_wait)       /* waiting for submitted GPU work */                \
    X(n_readback)        /* blocking buffer map-reads */                     \
    X(ns_readback)                                                           \
    X(n_tex_upload)                                                          \
    X(b_tex_upload)      /* bytes */                                         \
    X(n_surf_upload)                                                         \
    X(b_surf_upload)                                                         \
    X(n_surf_download)                                                       \
    X(b_surf_download)                                                       \
    X(n_present)                                                             \
    X(ns_present)        /* display render + yield to the browser */         \
    X(n_present_skipped) /* display ticks with nothing new to show */        \
    X(ns_draw)           /* GPU thread time building/encoding draws */       \
    /* disk (host file layer, thread-pool workers) */                        \
    X(n_disk_read)                                                           \
    X(b_disk_read)                                                           \
    X(n_disk_write)                                                          \
    X(b_disk_write)                                                          \
    X(ns_disk_io)        /* summed across worker threads */                  \
    /* audio */                                                              \
    X(n_apu_frame)       /* MCPX APU frames */                               \
    X(n_vblank)          /* guest VBLANKs raised (59.94 = NTSC realtime) */ \
    X(n_pit_tick)        /* PIT channel 0 output rising edges (timer IRQs) */ \
    X(n_rep_movs)        /* REP MOVS bulk-copy helper calls */ \
    X(b_rep_movs)        /* ... bytes copied by it */ \
    X(n_rep_movs_bail)   /* ... calls that left a remainder to the loop */ \
    /* guest-visible */                                                      \
    X(n_flip)            /* guest buffer flips (frames) */

typedef struct XemuWasmStats {
#define XEMU_WASM_STATS_DECL(name) uint64_t name;
    XEMU_WASM_STATS_FIELDS(XEMU_WASM_STATS_DECL)
#undef XEMU_WASM_STATS_DECL
} XemuWasmStats;

#ifdef EMSCRIPTEN
extern XemuWasmStats xemu_wasm_stats;
int64_t xemu_wasm_stats_now_ns(void);
/* per-MemoryRegion MMIO call count + time (vCPU thread), for the report */
void xemu_wasm_mmio_prof(const char *region, int64_t ns);
/* Named pfifo-thread events; key must be an interned string. */
void xemu_wasm_count(const char *key);
#define XSTAT_INC(f) (xemu_wasm_stats.f++)
#define XSTAT_ADD(f, v) (xemu_wasm_stats.f += (uint64_t)(v))
#define XSTAT_T0() int64_t xstat_t0_ = xemu_wasm_stats_now_ns()
#define XSTAT_T1(f) (xemu_wasm_stats.f += xemu_wasm_stats_now_ns() - xstat_t0_)

/*
 * Sampling profile: each profiled thread publishes what it is doing now as a
 * static string (NULL = its default: "jit" for the vCPU, "pfifo" for the GPU
 * thread); the main loop samples both every iteration (~1 kHz) and the page
 * reports the shares. Marks are one store each, so they can sit on hot paths.
 */
#define XPHASE_VCPU 0
#define XPHASE_GPU 1
extern const char *volatile xemu_wasm_phase[2];
void xemu_wasm_phase_sample(void);
#define XPHASE_PUSH(t, name) \
    const char *xphase_old_ = xemu_wasm_phase[t]; xemu_wasm_phase[t] = (name)
#define XPHASE_POP(t) (xemu_wasm_phase[t] = xphase_old_)
#define XPHASE_SET(t, name) (xemu_wasm_phase[t] = (name))
#else
#define XPHASE_PUSH(t, name) do { } while (0)
#define XPHASE_POP(t) do { } while (0)
#define XPHASE_SET(t, name) do { } while (0)
#define XSTAT_INC(f) do { } while (0)
#define XSTAT_ADD(f, v) do { } while (0)
#define XSTAT_T0() do { } while (0)
#define XSTAT_T1(f) do { } while (0)
#endif

#endif
