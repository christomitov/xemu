/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef QEMU_XEMU_WASM_CENSUS_H
#define QEMU_XEMU_WASM_CENSUS_H

/* Version 1: orthogonal TB requirements; mask zero is integer-only. */
enum {
    XWC_SSE = 1,
    XWC_X87 = 2,
    XWC_MMX = 4,
    XWC_SYSTEM = 8,
    XWC_STRING = 16,
    XWC_OTHER = 32,
    XWC_MASK = 63,
    XWC_ELIGIBLE = 64,
    XWC_REGONLY = 128,
    XWC_INSN_SHIFT = 8,
    XWC_COUNTER_MASK = (1u << 24) - 1,
    /* Translation-only metadata; never packed into the TCI counter word. */
    XWC_SELFLOOP = 1u << 24,
    XWC_NOCOUNT = 1u << 25,
};

/* Generated-code sampling classes, not instruction-count or idle claims. */
typedef enum XemuWasmInsnPhase {
    XWIP_ELIGIBLE,
    XWIP_ELIGIBLE_LOOP,
    XWIP_OTHER,
    XWIP_OTHER_LOOP,
    XWIP_UNKNOWN,
    XWIP_COUNT,
} XemuWasmInsnPhase;

/* 00..3f partition all entered TBs. 40/41 are overlapping subsets. */
#define XEMU_CENSUS_BUCKETS(X, A) \
    X(A, 00) X(A, 01) X(A, 02) X(A, 03) \
    X(A, 04) X(A, 05) X(A, 06) X(A, 07) \
    X(A, 08) X(A, 09) X(A, 0a) X(A, 0b) \
    X(A, 0c) X(A, 0d) X(A, 0e) X(A, 0f) \
    X(A, 10) X(A, 11) X(A, 12) X(A, 13) \
    X(A, 14) X(A, 15) X(A, 16) X(A, 17) \
    X(A, 18) X(A, 19) X(A, 1a) X(A, 1b) \
    X(A, 1c) X(A, 1d) X(A, 1e) X(A, 1f) \
    X(A, 20) X(A, 21) X(A, 22) X(A, 23) \
    X(A, 24) X(A, 25) X(A, 26) X(A, 27) \
    X(A, 28) X(A, 29) X(A, 2a) X(A, 2b) \
    X(A, 2c) X(A, 2d) X(A, 2e) X(A, 2f) \
    X(A, 30) X(A, 31) X(A, 32) X(A, 33) \
    X(A, 34) X(A, 35) X(A, 36) X(A, 37) \
    X(A, 38) X(A, 39) X(A, 3a) X(A, 3b) \
    X(A, 3c) X(A, 3d) X(A, 3e) X(A, 3f) \
    X(A, 40) X(A, 41)
#define XEMU_CENSUS_FIELDS_ONE(X, id) \
    X(n_census_tb_##id) X(n_census_insn_##id)
#define XEMU_WASM_CENSUS_FIELDS(X) \
    XEMU_CENSUS_BUCKETS(XEMU_CENSUS_FIELDS_ONE, X)

#endif
