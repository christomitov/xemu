/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * wasm32 JIT backend: TCG registers are 64 bits wide (wasm has native i64),
 * even though host pointers are 32 bits.
 */
#ifndef TCG_TARGET_REG_BITS_H
#define TCG_TARGET_REG_BITS_H

#define TCG_TARGET_REG_BITS  64

#endif
