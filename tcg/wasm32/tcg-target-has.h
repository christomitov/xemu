/* SPDX-License-Identifier: MIT */
/*
 * Define target-specific opcode support for the wasm32 JIT backend.
 * Same as TCI with 64-bit registers, minus test-conditions (TSTEQ/TSTNE),
 * which the wasm code generator does not implement.
 */
#ifndef TCG_TARGET_HAS_H
#define TCG_TARGET_HAS_H

#define TCG_TARGET_HAS_extr_i64_i32     0
#define TCG_TARGET_HAS_qemu_ldst_i128   0
#define TCG_TARGET_HAS_tst              0

#define TCG_TARGET_extract_valid(type, ofs, len)   1
#define TCG_TARGET_sextract_valid(type, ofs, len)  1
#define TCG_TARGET_deposit_valid(type, ofs, len)   1

#endif
