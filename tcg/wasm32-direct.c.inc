/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Independent bounded x86 decoder and i32-local emitter: no TCG IR input. */

typedef struct XwdReader {
    const uint8_t *bytes;
    unsigned pos, end;
    bool valid;
} XwdReader;

static uint32_t xwd_read(XwdReader *r, unsigned n)
{
    uint32_t value = 0;
    if (n > r->end - r->pos) {
        r->valid = false;
        return 0;
    }
    for (unsigned i = 0; i < n; i++) {
        value |= (uint32_t)r->bytes[r->pos++] << (8 * i);
    }
    return value;
}

static uint32_t xwd_signed_byte(XwdReader *r)
{
    return (uint32_t)(int32_t)(int8_t)xwd_read(r, 1);
}

static bool xwd_modrm(XwdReader *r, unsigned *reg, unsigned *rm)
{
    unsigned b = xwd_read(r, 1);
    *reg = (b >> 3) & 7;
    *rm = b & 7;
    return r->valid && (b >> 6) == 3;
}

static bool xwd_alu(unsigned group, XwdInsn *o)
{
    switch (group) {
    case 0:
        o->op = XWD_ADD;
        break;
    case 1:
        o->op = XWD_OR;
        break;
    case 2:
        o->op = XWD_ADC;
        break;
    case 3:
        o->op = XWD_SBB;
        break;
    case 4:
        o->op = XWD_AND;
        break;
    case 5:
        o->op = XWD_SUB;
        break;
    case 6:
        o->op = XWD_XOR;
        break;
    case 7:
        o->op = XWD_CMP;
        break;
    default:
        return false;
    }
    return true;
}

static bool xwd_lea(XwdReader *r, XwdInsn *o)
{
    unsigned b = xwd_read(r, 1), mod = b >> 6, rm = b & 7;
    if (mod == 3) {
        return false;
    }
    o->dst = (b >> 3) & 7;
    o->base = rm;
    o->index = 0xff;
    if (rm == 4) {
        b = xwd_read(r, 1);
        o->base = b & 7;
        o->index = (b >> 3) & 7;
        o->scale = b >> 6;
        if (o->index == 4) {
            o->index = 0xff;
        }
    }
    if (!mod && o->base == 5) {
        o->base = 0xff;
        o->imm = xwd_read(r, 4);
    } else if (mod == 1) {
        o->imm = xwd_signed_byte(r);
    } else if (mod == 2) {
        o->imm = xwd_read(r, 4);
    }
    return r->valid;
}

/* Return false for the WHOLE TB, before any direct instruction is emitted. */
static bool xwd_decode(const XwdCapture *c, XwdPlan *p)
{
    unsigned start = 0;
    bool terminal = false;

    memset(p, 0, sizeof(*p));
    if (!c->valid || c->version != XWD_VERSION || !c->count ||
        c->count > XWD_MAX_INSNS || !c->size || c->size > XWD_MAX_BYTES) {
        return false;
    }
    p->version = XWD_VERSION;
    p->pc = c->pc;
    p->pcrel = c->pcrel;
    p->count = c->count;
    p->delta = p->fall_delta = c->size;
    for (unsigned i = 0; i < c->count; i++) {
        XwdInsn *o = &p->insn[i];
        XwdReader r = { .bytes = c->bytes, .pos = start,
                        .end = c->end[i], .valid = true };
        unsigned reg, rm, b;
        bool prefix = false;
        if (terminal || r.end <= start || r.end > c->size ||
            r.end - start > 15) {
            return false;
        }
        start = r.end;
        o->width = 32;
        b = xwd_read(&r, 1);
        if (b == 0x66) {
            prefix = true;
            o->width = 16;
            b = xwd_read(&r, 1);
        }
        /* All other prefixes (including duplicates) decline. */
        if (b >= 0xb0 && b <= 0xbf) {
            o->op = XWD_MOV;
            o->dst = b & 7;
            if (b < 0xb8) {
                o->width = 8;
            }
            o->immediate = true;
            o->imm = xwd_read(&r, o->width / 8);
        } else if (b >= 0x88 && b <= 0x8b) {
            if (!xwd_modrm(&r, &reg, &rm)) {
                return false;
            }
            o->op = XWD_MOV;
            if (!(b & 1)) {
                o->width = 8;
            }
            o->dst = b & 2 ? reg : rm;
            o->src = b & 2 ? rm : reg;
        } else if (b == 0xc6 || b == 0xc7) {
            if (!xwd_modrm(&r, &reg, &rm) || reg != 0) {
                return false;
            }
            o->op = XWD_MOV;
            o->dst = rm;
            if (b == 0xc6) {
                o->width = 8;
            }
            o->immediate = true;
            o->imm = xwd_read(&r, o->width / 8);
        } else if (b <= 0x3d && (b & 7) <= 5) {
            if (!xwd_alu(b >> 3, o)) {
                return false;
            }
            if (!(b & 1)) {
                o->width = 8;
            }
            if ((b & 7) <= 3) {
                if (!xwd_modrm(&r, &reg, &rm)) {
                    return false;
                }
                o->dst = b & 2 ? reg : rm;
                o->src = b & 2 ? rm : reg;
            } else {
                o->immediate = true;
                o->imm = xwd_read(&r, o->width / 8);
            }
        } else if (b == 0x80 || b == 0x81 || b == 0x83) {
            if (!xwd_modrm(&r, &reg, &rm) || !xwd_alu(reg, o)) {
                return false;
            }
            o->dst = rm;
            if (b == 0x80) {
                o->width = 8;
            }
            o->immediate = true;
            o->imm = b == 0x83 ? xwd_signed_byte(&r) :
                                xwd_read(&r, o->width / 8);
        } else if (b == 0x84 || b == 0x85 || b == 0xa8 || b == 0xa9) {
            o->op = XWD_TEST;
            if (!(b & 1)) {
                o->width = 8;
            }
            if (b < 0xa0) {
                if (!xwd_modrm(&r, &reg, &rm)) {
                    return false;
                }
                o->dst = rm;
                o->src = reg;
            } else {
                o->immediate = true;
                o->imm = xwd_read(&r, o->width / 8);
            }
        } else if (b == 0xf6 || b == 0xf7) {
            if (!xwd_modrm(&r, &reg, &rm)) {
                return false;
            }
            o->dst = rm;
            if (b == 0xf6) {
                o->width = 8;
            }
            if (reg == 0) {
                o->op = XWD_TEST;
                o->immediate = true;
                o->imm = xwd_read(&r, o->width / 8);
            } else if (reg == 2 || reg == 3) {
                o->op = reg == 2 ? XWD_NOT : XWD_NEG;
            } else {
                return false;
            }
        } else if (b == 0x0f) {
            b = xwd_read(&r, 1);
            if (b >= 0x80 && b <= 0x8f && !prefix) {
                o->op = XWD_JCC;
                o->cond = b & 15;
                o->imm = xwd_read(&r, 4);
                p->delta = r.end + o->imm;
                p->conditional = terminal = true;
            } else {
                if (!xwd_modrm(&r, &reg, &rm)) {
                    return false;
                }
                o->dst = reg;
                o->src = rm;
                if (b >= 0x40 && b <= 0x4f) {
                    o->op = XWD_CMOV;
                    o->cond = b & 15;
                } else if (b >= 0x90 && b <= 0x9f) {
                    o->op = XWD_SETCC;
                    o->cond = b & 15;
                    o->width = 8;
                    o->dst = rm;
                } else if (b == 0xb6 || b == 0xb7 ||
                           b == 0xbe || b == 0xbf) {
                    o->op = b & 8 ? XWD_MOVSX : XWD_MOVZX;
                    o->src_width = b & 1 ? 16 : 8;
                } else {
                    return false;
                }
            }
        } else if (b >= 0x70 && b <= 0x7f && !prefix) {
            o->op = XWD_JCC;
            o->cond = b & 15;
            o->imm = xwd_signed_byte(&r);
            p->delta = r.end + o->imm;
            p->conditional = terminal = true;
        } else if (b >= 0x40 && b <= 0x4f) {
            o->op = b & 8 ? XWD_DEC : XWD_INC;
            o->dst = b & 7;
        } else if (b == 0xfe || b == 0xff) {
            if (!xwd_modrm(&r, &reg, &rm) || reg > 1) {
                return false;
            }
            o->op = reg ? XWD_DEC : XWD_INC;
            o->dst = rm;
            if (b == 0xfe) {
                o->width = 8;
            }
        } else if (b == 0xf8 || b == 0xf9 || b == 0xf5) {
            o->op = b == 0xf8 ? XWD_CLC : b == 0xf9 ? XWD_STC : XWD_CMC;
        } else if (b == 0x8d) {
            o->op = XWD_LEA;
            if (!xwd_lea(&r, o)) {
                return false;
            }
        } else if (b == 0x90) {
            o->op = XWD_NOP;
        } else if ((b >= 0x91 && b <= 0x97) || b == 0x86 || b == 0x87) {
            o->op = XWD_XCHG;
            o->src = b & 7;
            if (b < 0x90) {
                if (!xwd_modrm(&r, &reg, &rm)) {
                    return false;
                }
                o->dst = rm;
                o->src = reg;
                if (b == 0x86) {
                    o->width = 8;
                }
            }
        } else if ((b == 0xe9 || b == 0xeb) && !prefix) {
            o->op = XWD_JMP;
            o->imm = b == 0xeb ? xwd_signed_byte(&r) :
                                xwd_read(&r, 4);
            p->delta = r.end + o->imm;
            terminal = true;
        } else {
            return false;
        }
        if (!r.valid || r.pos != r.end) {
            return false;
        }
        p->needs_flags |= o->op >= XWD_ADC;
    }
    return start == c->size && (terminal || c->fallthrough);
}

/* Internal function ABI: (WasmContext*) -> i32, with private i32 locals. */
enum {
    XD_ENV = 1, XD_REG = 2, XD_DST = 10, XD_SRC = 11,
    XD_LEFT = 12, XD_RIGHT = 13, XD_RESULT = 14, XD_TMP = 15,
    XD_SRC2 = 16, XD_FLAGS = 17, XD_DELTA = 18,
};
typedef struct XwdEmitter {
    XwdCode *code;
    const XwdLayout *layout;
    uint8_t valid, dirty;
    bool flags_dirty, flags_cached, src2_dirty;
    uint32_t cc, cc_helper;
} XwdEmitter;

static void xwd_byte(XwdEmitter *e, uint8_t b)
{
    XwdCode *c = e->code;
    if (c->size == XWD_MAX_CODE) {
        c->valid = false;
    } else {
        c->bytes[c->size++] = b;
    }
}

static void xwd_uleb(XwdEmitter *e, uint32_t v)
{
    do {
        xwd_byte(e, (v & 127) | (v > 127 ? 128 : 0));
        v >>= 7;
    } while (v);
}

static void xwd_const(XwdEmitter *e, uint32_t value)
{
    int64_t v = (int32_t)value;
    xwd_byte(e, 0x41);
    for (;;) {
        uint8_t b = v & 127;
        v >>= 7;
        if ((v == 0 && !(b & 64)) || (v == -1 && (b & 64))) {
            xwd_byte(e, b);
            break;
        }
        xwd_byte(e, b | 128);
    }
}

static void xwd_local(XwdEmitter *e, uint8_t op, unsigned n)
{
    xwd_byte(e, op);
    xwd_uleb(e, n);
}

static void xwd_mem(XwdEmitter *e, uint8_t op, unsigned off)
{
    xwd_byte(e, op);
    xwd_byte(e, op == 0x3a ? 0 : 2); /* byte store or i32 load/store */
    xwd_uleb(e, off);
}

static void xwd_mask(XwdEmitter *e, unsigned width)
{
    if (width != 32) {
        xwd_const(e, (1u << width) - 1);
        xwd_byte(e, 0x71);
    }
}

static unsigned xwd_reg(unsigned reg, unsigned width)
{
    return width == 8 ? reg & 3 : reg;
}

static void xwd_load_reg(XwdEmitter *e, unsigned reg)
{
    if (!(e->valid & (1u << reg))) {
        xwd_local(e, 0x20, XD_ENV);
        xwd_mem(e, 0x28, e->layout->regs + reg * 4);
        xwd_local(e, 0x21, XD_REG + reg);
        e->valid |= 1u << reg;
    }
}

static void xwd_get(XwdEmitter *e, unsigned reg, unsigned width)
{
    unsigned n = xwd_reg(reg, width);
    xwd_load_reg(e, n);
    xwd_local(e, 0x20, XD_REG + n);
    if (width == 8 && reg >= 4) {
        xwd_const(e, 8);
        xwd_byte(e, 0x76);
    }
    xwd_mask(e, width);
}

/* Value to write is in XD_RESULT, with low width bits significant. */
static void xwd_put(XwdEmitter *e, unsigned reg, unsigned width)
{
    unsigned n = xwd_reg(reg, width), shift = width == 8 && reg >= 4 ? 8 : 0;
    if (width != 32) {
        xwd_load_reg(e, n);
        xwd_local(e, 0x20, XD_REG + n);
        xwd_const(e, ~(((1u << width) - 1) << shift));
        xwd_byte(e, 0x71);
    }
    xwd_local(e, 0x20, XD_RESULT);
    xwd_mask(e, width);
    if (shift) {
        xwd_const(e, shift);
        xwd_byte(e, 0x74);
    }
    if (width != 32) {
        xwd_byte(e, 0x72);
    }
    xwd_local(e, 0x21, XD_REG + n);
    e->valid |= 1u << n;
    e->dirty |= 1u << n;
}

static void xwd_io(XwdEmitter *e, unsigned value)
{
    xwd_local(e, 0x20, XD_ENV);
    xwd_const(e, e->layout->can_do_io);
    xwd_byte(e, 0x6a);
    xwd_const(e, value);
    xwd_mem(e, 0x3a, 0);
}

/* Store address: architectural env (activate) or isolated scratch (verify). */
static void xwd_address(XwdEmitter *e, bool verify, uint32_t scratch)
{
    if (verify) {
        xwd_const(e, scratch);
    } else {
        xwd_local(e, 0x20, XD_ENV);
    }
}

/* Exact existing lazy-CC evaluator, bound directly as a pure Wasm import. */
static void xwd_flags(XwdEmitter *e)
{
    const XwdLayout *l = e->layout;
    const uint32_t off[] = { l->cc_dst, l->cc_src, l->cc_src2, l->cc_op };
    const unsigned local[] = { XD_DST, XD_SRC, XD_SRC2 };

    if (e->flags_cached) {
        return;
    }
    if (e->flags_dirty && e->cc == l->cc_eflags) {
        xwd_local(e, 0x20, XD_SRC);
    } else {
        for (unsigned n = 0; n < 4; n++) {
            if (e->flags_dirty && n < 3 && (n != 2 || e->src2_dirty)) {
                xwd_local(e, 0x20, local[n]);
            } else if (e->flags_dirty && n == 3) {
                xwd_const(e, e->cc);
            } else {
                xwd_local(e, 0x20, XD_ENV);
                xwd_mem(e, 0x28, off[n]);
            }
        }
        if (l->phase_ptr) {
            xwd_const(e, l->phase_ptr); xwd_const(e, l->phase_cc);
            xwd_mem(e, 0x36, 0);
        }
        xwd_byte(e, 0x10);
        xwd_uleb(e, e->cc_helper);
        if (l->phase_ptr) {
            xwd_const(e, l->phase_ptr); xwd_const(e, l->phase_generated);
            xwd_mem(e, 0x36, 0);
        }
    }
    xwd_local(e, 0x21, XD_FLAGS);
    e->flags_cached = true;
}

/* All sixteen x86 conditions; normalized i32 boolean on the value stack. */
static void xwd_condition(XwdEmitter *e, unsigned cond)
{
    static const uint32_t mask[] = { 0x800, 1, 0x40, 0x41, 0x80, 4 };
    unsigned base = cond >> 1;

    xwd_flags(e);
    xwd_local(e, 0x20, XD_FLAGS);
    if (base < 6) {
        xwd_const(e, mask[base]);
        xwd_byte(e, 0x71);
    } else {
        xwd_local(e, 0x20, XD_FLAGS);
        xwd_const(e, 4); xwd_byte(e, 0x76);
        xwd_byte(e, 0x73); /* SF xor (OF >> 4), in bit 7 */
        xwd_const(e, 0x80); xwd_byte(e, 0x71);
        if (base == 7) {
            xwd_local(e, 0x20, XD_FLAGS);
            xwd_const(e, 0x40); xwd_byte(e, 0x71);
            xwd_byte(e, 0x72);
        }
    }
    xwd_byte(e, 0x45);
    if (!(cond & 1)) {
        xwd_byte(e, 0x45);
    }
}

static bool xwd_emit(const XwdPlan *p, const XwdLayout *l, bool verify,
                     uint32_t scratch, uint32_t cc_helper, XwdCode *code)
{
    XwdEmitter e = { .code = code, .layout = l, .cc_helper = cc_helper };

    memset(code, 0, sizeof(*code));
    if (p->version != XWD_VERSION || !p->count || p->count > XWD_MAX_INSNS ||
        (p->needs_flags && cc_helper == UINT32_MAX)) {
        return false;
    }
    code->valid = true;
    /* The body includes its local declaration and final end. */
    xwd_byte(&e, 1); xwd_byte(&e, XD_DELTA); xwd_byte(&e, 0x7f);
    xwd_local(&e, 0x20, 0);
    xwd_mem(&e, 0x28, 0); /* WasmContext.env, unchanged external ABI */
    xwd_local(&e, 0x21, XD_ENV);
    for (unsigned i = 0; i < p->count; i++) {
        const XwdInsn *o = &p->insn[i];
        unsigned wi = o->width == 8 ? 0 : o->width == 16 ? 1 : 2;
        if (!verify && (!i || i + 1 == p->count)) {
            xwd_io(&e, i + 1 == p->count);
        }
        if (o->op == XWD_NOP || o->op == XWD_JMP) {
            continue;
        }
        if (o->op == XWD_JCC) {
            xwd_const(&e, p->delta);
            xwd_const(&e, p->fall_delta);
            xwd_condition(&e, o->cond);
            xwd_byte(&e, 0x1b);
            xwd_local(&e, 0x21, XD_DELTA);
            continue;
        }
        if (o->op == XWD_CLC || o->op == XWD_STC || o->op == XWD_CMC) {
            xwd_flags(&e);
            xwd_local(&e, 0x20, XD_FLAGS);
            xwd_const(&e, o->op == XWD_CLC ? ~1u : 1);
            xwd_byte(&e, o->op == XWD_CLC ? 0x71 :
                         o->op == XWD_STC ? 0x72 : 0x73);
            xwd_local(&e, 0x22, XD_FLAGS);
            xwd_local(&e, 0x21, XD_SRC);
            e.cc = l->cc_eflags;
            e.flags_dirty = true;
            continue;
        }
        if (o->op == XWD_LEA) {
            xwd_const(&e, o->imm);
            if (o->base != 0xff) {
                xwd_get(&e, o->base, 32);
                xwd_byte(&e, 0x6a);
            }
            if (o->index != 0xff) {
                xwd_get(&e, o->index, 32);
                xwd_const(&e, o->scale);
                xwd_byte(&e, 0x74);
                xwd_byte(&e, 0x6a);
            }
        } else if (o->op == XWD_MOVZX || o->op == XWD_MOVSX) {
            xwd_get(&e, o->src, o->src_width);
            if (o->op == XWD_MOVSX) {
                xwd_byte(&e, o->src_width == 8 ? 0xc0 : 0xc1);
            }
        } else if (o->op == XWD_CMOV) {
            xwd_get(&e, o->src, o->width);
            xwd_get(&e, o->dst, o->width);
            xwd_condition(&e, o->cond);
            xwd_byte(&e, 0x1b);
        } else if (o->op == XWD_SETCC) {
            xwd_condition(&e, o->cond);
        } else if (o->op == XWD_MOV) {
            if (o->immediate) {
                xwd_const(&e, o->imm);
            } else {
                xwd_get(&e, o->src, o->width);
            }
        } else {
            xwd_get(&e, o->dst, o->width);
            xwd_local(&e, 0x21, XD_LEFT);
            if (o->op == XWD_NOT || o->op == XWD_NEG) {
                xwd_local(&e, 0x20, XD_LEFT);
            } else if (o->op == XWD_INC || o->op == XWD_DEC) {
                xwd_const(&e, 1);
            } else if (o->immediate) {
                xwd_const(&e, o->imm);
                xwd_mask(&e, o->width);
            } else {
                xwd_get(&e, o->src, o->width);
            }
            xwd_local(&e, 0x21, XD_RIGHT);
            if (o->op == XWD_XCHG) {
                xwd_local(&e, 0x20, XD_LEFT);
                xwd_local(&e, 0x21, XD_RESULT);
                xwd_put(&e, o->src, o->width);
                xwd_local(&e, 0x20, XD_RIGHT);
            } else {
                /* Read old carry BEFORE replacing its lazy-CC operands. */
                if (o->op == XWD_ADC || o->op == XWD_SBB ||
                    o->op == XWD_INC || o->op == XWD_DEC) {
                    xwd_flags(&e);
                    xwd_local(&e, 0x20, XD_FLAGS);
                    xwd_const(&e, 1); xwd_byte(&e, 0x71);
                    if (o->op == XWD_ADC || o->op == XWD_SBB) {
                        xwd_local(&e, 0x21, XD_SRC2);
                        e.src2_dirty = true;
                    } else {
                        xwd_local(&e, 0x21, XD_SRC);
                    }
                }
                if (o->op != XWD_NOT && o->op != XWD_INC && o->op != XWD_DEC) {
                    xwd_local(&e, 0x20, XD_RIGHT);
                    xwd_local(&e, 0x21, XD_SRC);
                }
                if (o->op == XWD_NEG) {
                    xwd_const(&e, 0);
                } else {
                    xwd_local(&e, 0x20, XD_LEFT);
                }
                xwd_local(&e, 0x20, XD_RIGHT);
                switch (o->op) {
                case XWD_ADD:
                    xwd_byte(&e, 0x6a);
                    e.cc = l->cc_add[wi];
                    break;
                case XWD_SUB:
                case XWD_CMP:
                case XWD_NEG:
                    xwd_byte(&e, 0x6b);
                    e.cc = l->cc_sub[wi];
                    break;
                case XWD_ADC:
                case XWD_SBB:
                    xwd_byte(&e, o->op == XWD_ADC ? 0x6a : 0x6b);
                    xwd_local(&e, 0x20, XD_SRC2);
                    xwd_byte(&e, o->op == XWD_ADC ? 0x6a : 0x6b);
                    e.cc = o->op == XWD_ADC ? l->cc_adc[wi] : l->cc_sbb[wi];
                    break;
                case XWD_INC:
                case XWD_DEC:
                    xwd_byte(&e, o->op == XWD_INC ? 0x6a : 0x6b);
                    e.cc = o->op == XWD_INC ? l->cc_inc[wi] : l->cc_dec[wi];
                    break;
                case XWD_AND:
                case XWD_TEST:
                    xwd_byte(&e, 0x71);
                    e.cc = l->cc_logic[wi];
                    break;
                case XWD_OR:
                    xwd_byte(&e, 0x72);
                    e.cc = l->cc_logic[wi];
                    break;
                case XWD_XOR:
                    xwd_byte(&e, 0x73);
                    e.cc = l->cc_logic[wi];
                    break;
                case XWD_NOT:
                    /* Drop the duplicate right operand before complement. */
                    xwd_byte(&e, 0x1a);
                    xwd_const(&e, UINT32_MAX);
                    xwd_byte(&e, 0x73);
                    break;
                default:
                    return false;
                }
                if (o->op != XWD_NOT) {
                    xwd_mask(&e, o->width);
                    xwd_local(&e, 0x22, XD_DST);
                    e.flags_dirty = true;
                    e.flags_cached = false;
                }
            }
        }
        xwd_local(&e, 0x21, XD_RESULT);
        if (o->op != XWD_CMP && o->op != XWD_TEST) {
            xwd_put(&e, o->dst, o->width);
        }
    }
    for (unsigned n = 0; n < 8; n++) {
        if (verify || (e.dirty & (1u << n))) {
            xwd_load_reg(&e, n);
            xwd_address(&e, verify, scratch);
            xwd_local(&e, 0x20, XD_REG + n);
            xwd_mem(&e, 0x36, verify ? offsetof(XwdState, regs) + n * 4 :
                                     l->regs + n * 4);
        }
    }
    const uint32_t src_off[] = { l->cc_dst, l->cc_src, l->cc_src2, l->cc_op };
    const uint32_t dst_off[] = { offsetof(XwdState, cc_dst),
        offsetof(XwdState, cc_src), offsetof(XwdState, cc_src2),
        offsetof(XwdState, cc_op) };
    for (unsigned n = 0; n < 4; n++) {
        if (!verify && (!e.flags_dirty || (n == 2 && !e.src2_dirty))) {
            continue;
        }
        xwd_address(&e, verify, scratch);
        if (e.flags_dirty && (n == 0 || n == 1)) {
            xwd_local(&e, 0x20, n == 0 ? XD_DST : XD_SRC);
        } else if (e.flags_dirty && n == 2 && e.src2_dirty) {
            xwd_local(&e, 0x20, XD_SRC2);
        } else if (e.flags_dirty && n == 3) {
            xwd_const(&e, e.cc);
        } else {
            xwd_local(&e, 0x20, XD_ENV);
            xwd_mem(&e, 0x28, src_off[n]);
        }
        xwd_mem(&e, 0x36, verify ? dst_off[n] : src_off[n]);
    }
    xwd_address(&e, verify, scratch);
    if (p->pcrel) {
        xwd_local(&e, 0x20, XD_ENV);
        xwd_mem(&e, 0x28, l->eip);
    } else {
        xwd_const(&e, p->pc);
    }
    if (p->conditional) {
        xwd_local(&e, 0x20, XD_DELTA);
    } else {
        xwd_const(&e, p->delta);
    }
    xwd_byte(&e, 0x6a);
    xwd_mem(&e, 0x36, verify ? offsetof(XwdState, eip) : l->eip);
    if (verify) {
        xwd_const(&e, scratch);
        xwd_const(&e, 1);
        xwd_mem(&e, 0x36, offsetof(XwdState, pending));
        xwd_const(&e, scratch);
        xwd_const(&e, p->needs_flags | (p->conditional << 1));
        xwd_mem(&e, 0x36, offsetof(XwdState, features));
    }
    xwd_const(&e, 0);
    xwd_byte(&e, 0x0b);
    return code->valid;
}
