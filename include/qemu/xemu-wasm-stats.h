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

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include "qemu/xemu-wasm-census.h"

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
    X(n_co_allocated)    /* allocated Wasm coroutine fibers */              \
    X(n_co_freed)        /* deleted Wasm coroutine fibers */                \
    X(n_co_live)         /* allocated fibers, including idle pool (gauge) */ \
    X(n_co_peak)         /* peak allocated fibers (gauge) */                \
    X(n_co_pooled)       /* idle fibers across all pools (gauge) */         \
    X(b_co_live)         /* fiber structs + two stacks (bytes gauge) */     \
    X(n_co_leaders)      /* per-thread coroutine leaders (gauge) */         \
    X(b_co_leader)       /* leader structs + Asyncify stacks (gauge) */     \
    X(n_jit_entries)     /* XEMU_WASM_JIT_ENTRY_PROF: dispatcher calls */     \
    X(n_jit_alias_entries) /* ... through a shared non-root member */        \
    /* Same opt-in flag: generated returns, not internal TB transitions. */ \
    X(n_jit_exit_direct) /* live goto_tb target outside self/region/IC */    \
    X(n_jit_exit_ptr)    /* nonnull goto_ptr outside self/region/IC */       \
    X(n_jit_exit_ptr_null) /* goto_ptr lookup failed: back to cpu-exec */    \
    X(n_jit_exit_chain0) /* exit_tb with an unchained slot 0 */              \
    X(n_jit_exit_chain1) /* exit_tb with an unchained slot 1 */              \
    X(n_jit_exit_requested) /* exit_tb: icount/interrupt request */          \
    X(n_jit_exit_plain)  /* exit_tb(NULL, 0), no chain proposed */           \
    X(n_jit_exit_other)  /* unexpected exit_tb reason, still accounted */    \
    X(n_jit_noreturn)    /* auxiliary: NO_RETURN helper calls, not returns */ \
    X(n_jit_evict)       /* modules dropped at the instance cap */           \
    X(n_jit_relink)      /* modules re-instantiated with fresh links */      \
    X(n_tb_linked)       /* TB transitions via a successor link */           \
    X(n_tb_selfloop)     /* goto_tb to the same TB (wasm loop) */            \
    X(n_tb_gotoptr)      /* indirect: goto_ptr after lookup_tb_ptr */        \
    X(n_tb_linkmiss)     /* chained goto_tb whose link was missing/stale */  \
    X(n_ic_direct_hit)   /* direct exits tail-called across modules */      \
    X(n_ic_direct_miss)  /* direct exits returned to the dispatcher */      \
    X(n_ic_indirect_hit) /* validated goto_ptr exits tail-called */         \
    X(n_ic_indirect_miss) /* goto_ptr exits returned to the dispatcher */   \
    X(n_ic_fill)         /* dispatcher updates to module cache tables */    \
    X(n_ic_rewind)       /* Asyncify forwards to another module's globals */ \
    X(n_guest_insn)      /* guest insns in dispatched JIT TBs (not loops) */  \
    X(n_tb_invalidate)   /* TBs invalidated (SMC, page writes) */            \
    X(n_bind_group_create) /* WebGPU bind groups created (cache misses) */  \
    X(n_slow_st)         /* stores taking the softmmu slow path */          \
    X(n_slow_st_miss)    /*  ... TLB miss (victim hit or fill) */          \
    X(n_slow_st_notdirty) /* ... clean page (code / dirty tracking) */     \
    X(n_smc_bitmap_miss) /* ... code-page stores cleared by the code bitmap */ \
    X(n_smc_drop_pages)  /* speculative whole-page code-cache evictions */ \
    X(n_smc_drop_busy)   /* current/unknown TB: speculative drop declined */ \
    X(n_smc_drop_changed) /* code coverage changed before drop recheck */ \
    X(n_notdirty_fast_skip) /* opt-in: protected-page TLB cleanup elided */  \
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
    X(n_region_shared)   /* stable multi-entry region instances */           \
    X(n_region_aliases)  /* additional headers sharing those instances */    \
    X(n_region_cap)      /* shared entries rejected by final CFG/byte cap */ \
    X(n_region_bytes_max) /* largest accepted shared entry function */       \
    X(n_region_cfg_max)  /* largest accepted shared CFG score */             \
    X(n_region_depth_max) /* largest accepted shared entry nesting */        \
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
    X(n_eager_readback)  /* small surfaces copied back early */ \
    X(n_eager_readback_hit) /* ... whose early copy served a CPU read */ \
    X(n_wgsl_cache_hit)  /* shaders whose WGSL came from the persistent cache */ \
    /* guest-visible */                                                      \
    X(n_flip)            /* guest buffer flips (frames) */                   \
    XEMU_WASM_CENSUS_FIELDS(X)

typedef struct XemuWasmStats {
#define XEMU_WASM_STATS_DECL(name) uint64_t name;
    XEMU_WASM_STATS_FIELDS(XEMU_WASM_STATS_DECL)
#undef XEMU_WASM_STATS_DECL
} XemuWasmStats;

#ifdef EMSCRIPTEN
extern XemuWasmStats xemu_wasm_stats;

/* Rollout: unset retains legacy telemetry; explicit 0 tests the lean path. */
static inline bool xemu_wasm_tb_stats_enabled(void)
{
    static int enabled = -1;

    if (enabled < 0) {
        const char *e = getenv("XEMU_WASM_TB_STATS");
        enabled = !(e && *e == '0');
    }
    return enabled;
}

/* Explicit JIT attribution needs the sampler even with TB_STATS=0. */
static inline bool xemu_wasm_profile_enabled(void)
{
    static int enabled = -1;

    if (enabled < 0) {
        const char *p = getenv("XEMU_WASM_PROFILE");
        const char *j = getenv("XEMU_WASM_JIT_PROFILE");
        enabled = (j && (atoi(j) & 15)) ||
                  (p ? *p == '1' : xemu_wasm_tb_stats_enabled());
    }
    return enabled;
}

#define XTBSTAT_INC(f) do { \
    if (xemu_wasm_tb_stats_enabled()) { \
        XSTAT_INC(f); \
    } \
} while (0)
#define XTBSTAT_ADD(f, n) do { \
    if (xemu_wasm_tb_stats_enabled()) { \
        XSTAT_ADD(f, n); \
    } \
} while (0)
#define XTBPHASE_SET(name) do { \
    if (xemu_wasm_profile_enabled()) { \
        XPHASE_SET(XPHASE_VCPU, name); \
    } \
} while (0)

/* Single vCPU writer, like the other generated counters; no clocks/calls. */
static inline uint64_t *xemu_wasm_census_counter(unsigned bucket, bool insns)
{
#define XEMU_CENSUS_PTRS(unused, id) \
    { &xemu_wasm_stats.n_census_tb_##id, \
      &xemu_wasm_stats.n_census_insn_##id },
    static uint64_t *const counters[][2] = {
        XEMU_CENSUS_BUCKETS(XEMU_CENSUS_PTRS, unused)
    };
#undef XEMU_CENSUS_PTRS
    return counters[bucket][insns];
}

static inline void xemu_wasm_census_hit(uint32_t word)
{
    unsigned bucket = word & XWC_MASK;
    unsigned n = word >> XWC_INSN_SHIFT;

    ++*xemu_wasm_census_counter(bucket, false);
    *xemu_wasm_census_counter(bucket, true) += n;
    if (word & XWC_ELIGIBLE) {
        ++*xemu_wasm_census_counter(0x40, false);
        *xemu_wasm_census_counter(0x40, true) += n;
        if (word & XWC_REGONLY) {
            ++*xemu_wasm_census_counter(0x41, false);
            *xemu_wasm_census_counter(0x41, true) += n;
        }
    }
}

int64_t xemu_wasm_stats_now_ns(void);
/* per-MemoryRegion MMIO call count + time (vCPU thread), for the report */
void xemu_wasm_mmio_prof(const char *region, int64_t ns);
/* Named pfifo-thread events; key must be an interned string. */
void xemu_wasm_count(const char *key);
void xemu_wasm_count_add(const char *key, uint32_t n);
/* the last key this thread passed to xemu_wasm_count() */
extern __thread const char *xemu_wasm_last_count_key;
#define XSTAT_INC(f) (xemu_wasm_stats.f++)
#define XSTAT_ADD(f, v) (xemu_wasm_stats.f += (uint64_t)(v))
#define XSTAT_T0() int64_t xstat_t0_ = xemu_wasm_stats_now_ns()
#define XSTAT_T1(f) (xemu_wasm_stats.f += xemu_wasm_stats_now_ns() - xstat_t0_)

/*
 * Sampling profile: each profiled thread publishes what it is doing now as a
 * static string (NULL = its default: "jit" for the vCPU, "pfifo" for the GPU
 * thread); a dedicated sampler thread samples both (~1 kHz) and the page
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

/* JIT_PROFILE bit 4: C notdirty subphases, without per-store clocks. */
static inline bool xemu_wasm_smc_profile_enabled(void)
{
    static int enabled = -1;

    if (enabled < 0) {
        const char *e = getenv("XEMU_WASM_JIT_PROFILE");
        enabled = e && (atoi(e) & 4);
    }
    return enabled;
}

#define XSMC_PROF_BEGIN(name) \
    const bool xsmc_prof_ = xemu_wasm_smc_profile_enabled(); \
    const char *xsmc_old_ = xsmc_prof_ ? xemu_wasm_phase[XPHASE_VCPU] : NULL; \
    XSMC_PROF_SET(name)
#define XSMC_PROF_SET(name) do { \
    if (xsmc_prof_) { \
        XPHASE_SET(XPHASE_VCPU, name); \
    } \
} while (0)
#define XSMC_PROF_END() do { \
    if (xsmc_prof_) { \
        XPHASE_SET(XPHASE_VCPU, xsmc_old_); \
    } \
} while (0)
#else
#define XPHASE_PUSH(t, name) do { } while (0)
#define XPHASE_POP(t) do { } while (0)
#define XPHASE_SET(t, name) do { } while (0)
#define XTBSTAT_INC(f) do { } while (0)
#define XTBSTAT_ADD(f, n) do { } while (0)
#define XTBPHASE_SET(name) do { } while (0)
#define XSMC_PROF_BEGIN(name) do { } while (0)
#define XSMC_PROF_SET(name) do { } while (0)
#define XSMC_PROF_END() do { } while (0)
#define XSTAT_INC(f) do { } while (0)
#define XSTAT_ADD(f, v) do { } while (0)
#define XSTAT_T0() do { } while (0)
#define XSTAT_T1(f) do { } while (0)
#endif

#endif
