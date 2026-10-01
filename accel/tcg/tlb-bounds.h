/*
 * softmmu size bounds
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#ifndef ACCEL_TCG_TLB_BOUNDS_H
#define ACCEL_TCG_TLB_BOUNDS_H

#ifdef EMSCRIPTEN
/*
 * The TLB only resizes on a flush, and the Xbox rarely flushes it: at the
 * default 256 entries the intro took ~190k victim-TLB/fill slow paths/s
 * (each a helper call out of the JIT code). Start large and stay large.
 */
#define CPU_TLB_DYN_MIN_BITS 10
#define CPU_TLB_DYN_MAX_BITS (32 - TARGET_PAGE_BITS)
#define CPU_TLB_DYN_DEFAULT_BITS 12
#else
#define CPU_TLB_DYN_MIN_BITS 6
#define CPU_TLB_DYN_MAX_BITS (32 - TARGET_PAGE_BITS)
#define CPU_TLB_DYN_DEFAULT_BITS 8
#endif

#endif /* ACCEL_TCG_TLB_BOUNDS_H */
