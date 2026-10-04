/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef QEMU_XEMU_WASM_DIRECT_MEMORY_H
#define QEMU_XEMU_WASM_DIRECT_MEMORY_H

/* Translation-pool metadata only. Never serialize this struct's padding or
 * pointers. Runtime memory admission/continuations are a separate stage.
 */
#define XWD_MAX_MEMOPS XWD_MAX_INSNS

typedef struct XwdMemorySite {
    uint32_t insn, oi, restore_pc;
    int32_t mask_ofs, table_ofs;
    uint32_t cmp_ofs, addend_ofs, index_shift, page_mask, a_mask, s_mask;
    uint32_t data_reg, addr_reg, store;
} XwdMemorySite;

typedef struct XwdMemoryCapture {
    uint32_t insn, restore_pc, count;
    bool valid;
    XwdMemorySite site[XWD_MAX_MEMOPS];
} XwdMemoryCapture;

static inline bool xemu_wasm_direct_memory_enabled(void)
{
#if defined(EMSCRIPTEN) && defined(CONFIG_TCG_WASM_JIT)
    static int enabled = -1;
    if (enabled < 0) {
        const char *e = getenv("XEMU_WASM_DIRECT_MEMORY");
        enabled = e && *e == '1';
    }
    return enabled;
#else
    return false;
#endif
}

#endif
