/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Versioned, bounded, little-endian value encoding in xemu.direct-region.
 * No translation-pool pointers, native padding or bool representations escape.
 * Function words are main-module Wasm table indices, never guest addresses.
 * Include after wasm32-direct.c.inc; native fixtures use synthetic indices.
 */
/* Version2 certifies that all resumable sites have DIRECT_SUSPEND relocs. */
#define XWD_BLUEPRINT_VERSION 4u   /* 3: + cc_shl, cc_sar; 4: + FS/GS bases */
#define XWD_BLUEPRINT_NAME "xemu.direct-region"

#define XWD_LAYOUT_SCALARS(F) \
    F(regs) F(eip) F(cc_dst) F(cc_src) F(cc_src2) F(cc_op) \
    F(can_do_io) F(icount_decr) F(cc_eflags) \
    F(phase_ptr) F(phase_cc) F(phase_generated) \
    F(seg_fs_base) F(seg_gs_base)
#define XWD_LAYOUT_ARRAYS(F) \
    F(cc_add) F(cc_sub) F(cc_logic) F(cc_adc) F(cc_sbb) F(cc_inc) F(cc_dec) \
    F(cc_shl) F(cc_sar)

static inline void xwd_blueprint_word(XwdEmitter *e, uint32_t v)
{
    for (unsigned i = 0; i < 4; i++) {
        xwd_byte(e, v >> (8 * i));
    }
}

static inline bool xwd_blueprint_write_ex(const XwdTranslation *t, XwdCode *code,
                                           bool allow_memory)
{
    const XwdCapture *c = &t->capture;
    const XwdLayout *l = &t->layout;
    XwdEmitter e = { .code = code };
    XwdPlan plan;

    memset(code, 0, sizeof(*code));
    if (!l->verify || !l->cc_all || c->jump_valid > 3 ||
        (uintptr_t)l->verify > UINT32_MAX ||
        (uintptr_t)l->cc_all > UINT32_MAX ||
        !xwd_decode_ex(c, &plan, allow_memory)) {
        return false;
    }
    code->valid = true;
    xwd_blueprint_word(&e, XWD_BLUEPRINT_VERSION);
    xwd_blueprint_word(&e, c->version);
    xwd_blueprint_word(&e, c->pc);
    xwd_blueprint_word(&e, c->size);
    xwd_blueprint_word(&e, c->count);
    xwd_blueprint_word(&e, c->fallthrough | (c->pcrel << 1));
    xwd_blueprint_word(&e, c->jump_valid);
    xwd_blueprint_word(&e, c->jump_pc[0]);
    xwd_blueprint_word(&e, c->jump_pc[1]);
    xwd_blueprint_word(&e, (uintptr_t)l->verify);
    xwd_blueprint_word(&e, (uintptr_t)l->cc_all);
#define XWD_WRITE_FIELD(n) xwd_blueprint_word(&e, l->n);
    XWD_LAYOUT_SCALARS(XWD_WRITE_FIELD)
#undef XWD_WRITE_FIELD
#define XWD_WRITE_ARRAY(n) \
    for (unsigned i = 0; i < 3; i++) { \
        xwd_blueprint_word(&e, l->n[i]); \
    }
    XWD_LAYOUT_ARRAYS(XWD_WRITE_ARRAY)
#undef XWD_WRITE_ARRAY
    for (unsigned i = 0; i < c->count; i++) {
        xwd_blueprint_word(&e, c->end[i]);
    }
    for (unsigned i = 0; i < c->size; i++) {
        xwd_byte(&e, c->bytes[i]);
    }
    return code->valid;
}

static inline bool xwd_blueprint_write(const XwdTranslation *t, XwdCode *code)
{
    return xwd_blueprint_write_ex(t, code, false);
}

static inline bool xwd_blueprint_read_ex(const uint8_t *bytes, unsigned len,
                                         XwdTranslation *t, XwdPlan *plan,
                                         bool allow_memory)
{
    XwdReader r = { .bytes = bytes, .end = len, .valid = true };
    XwdCapture *c = &t->capture;
    XwdLayout *l = &t->layout;
    uint32_t size, count, flags, jumps;

    memset(t, 0, sizeof(*t));
    memset(plan, 0, sizeof(*plan));
    if (!bytes || len > XWD_MAX_CODE ||
        xwd_read(&r, 4) != XWD_BLUEPRINT_VERSION ||
        xwd_read(&r, 4) != XWD_VERSION) {
        return false;
    }
    c->version = XWD_VERSION;
    c->pc = xwd_read(&r, 4);
    size = xwd_read(&r, 4);
    count = xwd_read(&r, 4);
    flags = xwd_read(&r, 4);
    jumps = xwd_read(&r, 4);
    if (size > XWD_MAX_BYTES || !count || count > XWD_MAX_INSNS ||
        flags > 3 || jumps > 3) {
        return false;
    }
    c->size = size;
    c->count = count;
    c->fallthrough = flags & 1;
    c->pcrel = flags & 2;
    c->jump_valid = jumps;
    c->jump_pc[0] = xwd_read(&r, 4);
    c->jump_pc[1] = xwd_read(&r, 4);
    l->verify = (void *)(uintptr_t)xwd_read(&r, 4);
    l->cc_all = (void *)(uintptr_t)xwd_read(&r, 4);
#define XWD_READ_FIELD(n) l->n = xwd_read(&r, 4);
    XWD_LAYOUT_SCALARS(XWD_READ_FIELD)
#undef XWD_READ_FIELD
#define XWD_READ_ARRAY(n) \
    for (unsigned i = 0; i < 3; i++) { \
        l->n[i] = xwd_read(&r, 4); \
    }
    XWD_LAYOUT_ARRAYS(XWD_READ_ARRAY)
#undef XWD_READ_ARRAY
    for (unsigned i = 0; i < count; i++) {
        uint32_t end = xwd_read(&r, 4);
        if (end > size || (i && end <= c->end[i - 1])) {
            return false;
        }
        c->end[i] = end;
    }
    for (unsigned i = 0; i < size; i++) {
        c->bytes[i] = xwd_read(&r, 1);
    }
    c->valid = r.valid && r.pos == r.end;
    if (!c->valid || !l->verify || !l->cc_all ||
        !xwd_decode_ex(c, plan, allow_memory)) {
        c->valid = false;
        memset(plan, 0, sizeof(*plan));
        return false;
    }
    return true;
}

static inline bool xwd_blueprint_read(const uint8_t *bytes, unsigned len,
                                      XwdTranslation *t, XwdPlan *plan)
{
    return xwd_blueprint_read_ex(bytes, len, t, plan, false);
}

/* Wasm permits padded ULEBs, but never more than 32 payload bits. */
static inline bool xwd_blueprint_uleb(XwdReader *r, uint32_t *value)
{
    uint32_t v = 0;

    for (unsigned shift = 0; shift < 35; shift += 7) {
        uint32_t b = xwd_read(r, 1);
        if (!r->valid || (shift == 28 && (b & 0xf0))) {
            return false;
        }
        v |= (b & 0x7f) << shift;
        if (!(b & 0x80)) {
            *value = v;
            return true;
        }
    }
    return false;
}

/*
 * Scan copied module bytes, not a header or translation-pool pointer cache.
 * Layout words/table indices still require authoritative builder compatibility
 * checks; this parser validates framing and instruction admission only.
 */
static inline const uint8_t *xwd_custom_find(const uint8_t *bytes, unsigned len,
                                             const char *wanted,
                                             unsigned *payload_len)
{
    static const uint8_t magic[] = { 0, 'a', 's', 'm', 1, 0, 0, 0 };
    const uint8_t *payload = NULL;
    unsigned wanted_len = strlen(wanted);
    XwdReader r = { .bytes = bytes, .pos = sizeof(magic), .end = len,
                    .valid = true };

    *payload_len = 0;
    if (!bytes || len < sizeof(magic) || memcmp(bytes, magic, sizeof(magic))) {
        return false;
    }
    while (r.pos < r.end) {
        unsigned id = xwd_read(&r, 1);
        uint32_t section_len;
        if (!xwd_blueprint_uleb(&r, &section_len) ||
            section_len > r.end - r.pos) {
            return false;
        }
        unsigned end = r.pos + section_len;
        if (id == 0) {
            XwdReader name = r;
            uint32_t name_len;
            name.end = end;
            if (!xwd_blueprint_uleb(&name, &name_len) ||
                name_len > name.end - name.pos) {
                return false;
            }
            if (name_len == wanted_len &&
                !memcmp(bytes + name.pos, wanted, name_len)) {
                if (payload) {
                    return false; /* Ambiguous or duplicate direct metadata. */
                }
                payload = bytes + name.pos + name_len;
                *payload_len = end - name.pos - name_len;
            }
        }
        r.pos = end;
    }
    return payload;
}

static inline bool xwd_blueprint_find_ex(const uint8_t *bytes, unsigned len,
                                         XwdTranslation *t, XwdPlan *plan,
                                         bool allow_memory)
{
    unsigned size;
    const uint8_t *payload = xwd_custom_find(bytes, len, XWD_BLUEPRINT_NAME,
                                           &size);
    return xwd_blueprint_read_ex(payload, size, t, plan, allow_memory);
}

static inline bool xwd_blueprint_find(const uint8_t *bytes, unsigned len,
                                     XwdTranslation *t, XwdPlan *plan)
{
    return xwd_blueprint_find_ex(bytes, len, t, plan, false);
}

#undef XWD_LAYOUT_SCALARS
#undef XWD_LAYOUT_ARRAYS
