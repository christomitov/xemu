/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Value-only original-memory metadata. Include after the blueprint codec. */
#define XWD_MEMORY_NAME "xemu.direct-memory"
#define XWD_MEMORY_VERSION 1u
#define XWD_MEMORY_FIELDS(F) \
    F(insn) F(oi) F(restore_pc) F(mask_ofs) F(table_ofs) \
    F(cmp_ofs) F(addend_ofs) F(index_shift) F(page_mask) F(a_mask) F(s_mask) \
    F(data_reg) F(addr_reg) F(store)

static inline bool xwd_memory_write(const XwdMemoryCapture *c, XwdCode *code)
{
    XwdEmitter e = { .code = code };
    memset(code, 0, sizeof(*code));
    if (!c || !c->valid || c->count > XWD_MAX_MEMOPS) {
        return false;
    }
    code->valid = true;
    xwd_blueprint_word(&e, XWD_MEMORY_VERSION);
    xwd_blueprint_word(&e, c->count);
    for (unsigned i = 0; i < c->count; i++) {
        const XwdMemorySite *s = &c->site[i];
#define XWD_MEMORY_PUT(n) xwd_blueprint_word(&e, s->n);
        XWD_MEMORY_FIELDS(XWD_MEMORY_PUT)
#undef XWD_MEMORY_PUT
    }
    return code->valid;
}

static inline bool xwd_memory_find(const uint8_t *bytes, unsigned len,
                                   XwdMemoryCapture *c)
{
    unsigned size;
    const uint8_t *payload = xwd_custom_find(bytes, len, XWD_MEMORY_NAME, &size);
    XwdReader r = { .bytes = payload, .end = size, .valid = true };
    memset(c, 0, sizeof(*c));
    if (!payload || size > XWD_MAX_CODE ||
        xwd_read(&r, 4) != XWD_MEMORY_VERSION) {
        return false;
    }
    c->count = xwd_read(&r, 4);
    if (c->count > XWD_MAX_MEMOPS) {
        return false;
    }
    for (unsigned i = 0; i < c->count; i++) {
        XwdMemorySite *s = &c->site[i];
#define XWD_MEMORY_GET(n) s->n = xwd_read(&r, 4);
        XWD_MEMORY_FIELDS(XWD_MEMORY_GET)
#undef XWD_MEMORY_GET
        unsigned bits = (s->oi >> 5) & 7;
        if (s->insn >= XWD_MAX_INSNS || (i && s->insn <= c->site[i - 1].insn) ||
            bits > 2 || s->store > 1 || s->data_reg >= 16 ||
            s->addr_reg >= 16 || s->s_mask != (1u << bits) - 1 ||
            s->a_mask < s->s_mask || (s->a_mask & (s->a_mask + 1)) ||
            s->a_mask > 0xfff || s->page_mask != 0xfffff000u ||
            s->index_shift > 12 || s->cmp_ofs > 60 || s->addend_ofs > 60 ||
            (s->cmp_ofs & 3) || (s->addend_ofs & 3)) {
            return false;
        }
    }
    c->valid = r.valid && r.pos == r.end;
    return c->valid;
}
#undef XWD_MEMORY_FIELDS
