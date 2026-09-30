/*
 * Per-helper metadata TCI keeps right after each helper's ffi_cif
 * (see init_ffi_layout in tcg.c): the direct-call signature used by
 * tci-direct-call.c.inc and the helper's name (for the wasm helper
 * profile shown in the page's Stats report).
 */
#ifndef TCG_TCI_HELPER_META_H
#define TCG_TCI_HELPER_META_H

typedef struct TCIHelperMeta {
    unsigned direct_sig; /* UINT_MAX: use ffi_call */
    const char *name;
} TCIHelperMeta;

#endif
