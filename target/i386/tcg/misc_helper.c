/*
 *  x86 misc helpers
 *
 *  Copyright (c) 2003 Fabrice Bellard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "cpu.h"
#include "exec/helper-proto.h"
#include "exec/cputlb.h"
#include "helper-tcg.h"
#include "accel/tcg/cpu-ldst.h"
#include "accel/tcg/probe.h"
#include "exec/target_page.h"
#include "qemu/xemu-wasm-stats.h"

/*
 * NOTE: the translator must set DisasContext.cc_op to CC_OP_EFLAGS
 * after generating a call to a helper that uses this.
 */
void cpu_load_eflags(CPUX86State *env, int eflags, int update_mask)
{
    CC_SRC = eflags & (CC_O | CC_S | CC_Z | CC_A | CC_P | CC_C);
    CC_OP = CC_OP_EFLAGS;
    env->df = 1 - (2 * ((eflags >> 10) & 1));
    env->eflags = (env->eflags & ~update_mask) |
        (eflags & update_mask) | 0x2;
}

void helper_into(CPUX86State *env, int next_eip_addend)
{
    int eflags;

    eflags = cpu_cc_compute_all(env);
    if (eflags & CC_O) {
        raise_interrupt(env, EXCP04_INTO, next_eip_addend);
    }
}

void helper_cpuid(CPUX86State *env)
{
    uint32_t eax, ebx, ecx, edx;

    cpu_svm_check_intercept_param(env, SVM_EXIT_CPUID, 0, GETPC());

    cpu_x86_cpuid(env, (uint32_t)env->regs[R_EAX], (uint32_t)env->regs[R_ECX],
                  &eax, &ebx, &ecx, &edx);
    env->regs[R_EAX] = eax;
    env->regs[R_EBX] = ebx;
    env->regs[R_ECX] = ecx;
    env->regs[R_EDX] = edx;
}

void helper_rdtsc(CPUX86State *env)
{
    uint64_t val;

    if ((env->cr[4] & CR4_TSD_MASK) && ((env->hflags & HF_CPL_MASK) != 0)) {
        raise_exception_ra(env, EXCP0D_GPF, GETPC());
    }
    cpu_svm_check_intercept_param(env, SVM_EXIT_RDTSC, 0, GETPC());

    val = cpu_get_tsc(env) + env->tsc_offset;
    env->regs[R_EAX] = (uint32_t)(val);
    env->regs[R_EDX] = (uint32_t)(val >> 32);
}

G_NORETURN void helper_rdpmc(CPUX86State *env)
{
    if (((env->cr[4] & CR4_PCE_MASK) == 0 ) &&
        ((env->hflags & HF_CPL_MASK) != 0)) {
        raise_exception_ra(env, EXCP0D_GPF, GETPC());
    }
    cpu_svm_check_intercept_param(env, SVM_EXIT_RDPMC, 0, GETPC());

    /* currently unimplemented */
    qemu_log_mask(LOG_UNIMP, "x86: unimplemented rdpmc\n");
    raise_exception_err(env, EXCP06_ILLOP, 0);
}

G_NORETURN void helper_pause(CPUX86State *env)
{
    CPUState *cs = env_cpu(env);

    /* Do gen_eob() tasks before going back to the main loop.  */
    do_end_instruction(env);
    helper_rechecking_single_step(env);

    /* Just let another CPU run.  */
    cs->exception_index = EXCP_INTERRUPT;
    cpu_loop_exit(cs);
}

uint64_t helper_rdpkru(CPUX86State *env, uint32_t ecx)
{
    if ((env->cr[4] & CR4_PKE_MASK) == 0) {
        raise_exception_err_ra(env, EXCP06_ILLOP, 0, GETPC());
    }
    if (ecx != 0) {
        raise_exception_err_ra(env, EXCP0D_GPF, 0, GETPC());
    }

    return env->pkru;
}

void helper_wrpkru(CPUX86State *env, uint32_t ecx, uint64_t val)
{
    CPUState *cs = env_cpu(env);

    if ((env->cr[4] & CR4_PKE_MASK) == 0) {
        raise_exception_err_ra(env, EXCP06_ILLOP, 0, GETPC());
    }
    if (ecx != 0 || (val & 0xFFFFFFFF00000000ull)) {
        raise_exception_err_ra(env, EXCP0D_GPF, 0, GETPC());
    }

    env->pkru = val;
    tlb_flush(cs);
}

target_ulong HELPER(rdpid)(CPUX86State *env)
{
#if !defined CONFIG_USER_ONLY
    return env->tsc_aux;
#elif defined CONFIG_LINUX && defined CONFIG_GETCPU
    unsigned cpu, node;
    getcpu(&cpu, &node);
    return (node << 12) | (cpu & 0xfff);
#elif defined CONFIG_SCHED_GETCPU
    return sched_getcpu();
#else
    return 0;
#endif
}

#ifdef EMSCRIPTEN
#include <emscripten.h>
/* For the bench's guest-PC sampler: &env->eip and &env->segs[R_CS].base */
EMSCRIPTEN_KEEPALIVE uint32_t *xemu_wasm_eip_ptr(int which)
{
    CPUX86State *env = &X86_CPU(first_cpu)->env;
    return which ? (uint32_t *)&env->segs[R_CS].base : (uint32_t *)&env->eip;
}
#endif

/*
 * REP MOVS{B,W,D} with DF=0 and 32-bit addressing: copy page-sized chunks
 * with memmove instead of one guest load/store pair per element. Stops early
 * (leaving ECX/ESI/EDI at the next element) for MMIO, an element straddling
 * a page, or an overlap where a forward element copy differs from memmove;
 * the translated loop then finishes the job. probe_access() raises faults
 * and runs watchpoints, access callbacks and SMC invalidation for each chunk
 * before it is copied, exactly as the element stores would.
 */
void helper_rep_movs_fast(CPUX86State *env, uint32_t ot, uint32_t src_seg)
{
    static int enabled = -1;
    uintptr_t ra = GETPC();
    unsigned size = 1u << ot;
    int mmu_idx = cpu_mmu_index(env_cpu(env), false);

    if (enabled < 0) {
        const char *e = getenv("XEMU_WASM_REP_MOVS");
        enabled = !(e && *e == '0');
    }
    if (!enabled || env->df != 1) {
        return;
    }
    XSTAT_INC(n_rep_movs);
    XPHASE_PUSH(XPHASE_VCPU, "rep_movs_fast");
    while ((uint32_t)env->regs[R_ECX]) {
        uint32_t esi = env->regs[R_ESI], edi = env->regs[R_EDI];
        target_ulong src = env->segs[src_seg].base + esi;
        target_ulong dst = env->segs[R_ES].base + edi;
        uint64_t left = (uint64_t)(uint32_t)env->regs[R_ECX] * size;
        uint32_t src_room = TARGET_PAGE_SIZE - (src & ~TARGET_PAGE_MASK);
        uint32_t dst_room = TARGET_PAGE_SIZE - (dst & ~TARGET_PAGE_MASK);
        uint32_t n = MIN(left, MIN(src_room, dst_room));

        n &= ~(size - 1);
        if (n == 0 || (dst > src && dst < src + n)) {
            /* element straddles a page, or forward overlap: per element */
            XSTAT_INC(n_rep_movs_bail);
            break;
        }
        XPHASE_SET(XPHASE_VCPU, "rep_movs_probe");
        void *hs = probe_access(env, src, n, MMU_DATA_LOAD, mmu_idx, ra);
        void *hd = probe_access(env, dst, n, MMU_DATA_STORE, mmu_idx, ra);
        if (!hs || !hd) {
            XSTAT_INC(n_rep_movs_bail);
            break;      /* MMIO */
        }
        XPHASE_SET(XPHASE_VCPU, "rep_movs_copy");
        memmove(hd, hs, n);
        XSTAT_ADD(b_rep_movs, n);
        env->regs[R_ESI] = (uint32_t)(esi + n);
        env->regs[R_EDI] = (uint32_t)(edi + n);
        env->regs[R_ECX] = (uint32_t)env->regs[R_ECX] - n / size;
    }
    XPHASE_POP(XPHASE_VCPU);
}
