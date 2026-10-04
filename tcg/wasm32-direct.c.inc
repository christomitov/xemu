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

/* The memory operand of a ModRM byte: value comes from / goes to guest RAM. */
#define XWD_MEMREG 0xfe

/*
 * ModRM that may name memory: 1 for a register operand (*rm), 2 for a
 * memory operand (address in o->base/index/scale/disp, *rm = XWD_MEMREG),
 * 0 if invalid or memory is not allowed.
 */
static unsigned xwd_modrm_ex(XwdReader *r, unsigned *reg, unsigned *rm,
                             XwdInsn *o, bool allow_memory)
{
    unsigned b = xwd_read(r, 1), mod = b >> 6;

    *reg = (b >> 3) & 7;
    *rm = b & 7;
    if (!r->valid) {
        return 0;
    }
    if (mod == 3) {
        return 1;
    }
    if (!allow_memory) {
        return 0;
    }
    o->base = *rm;
    o->index = 0xff;
    o->scale = 0;
    o->disp = 0;
    if (*rm == 4) {
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
        o->disp = xwd_read(r, 4);
    } else if (mod == 1) {
        o->disp = xwd_signed_byte(r);
    } else if (mod == 2) {
        o->disp = xwd_read(r, 4);
    }
    o->mem = true;
    *rm = XWD_MEMREG;
    return r->valid ? 2 : 0;
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

/*
 * Return false for the WHOLE TB, before any direct instruction is emitted.
 * allow_memory admits the memory-operand forms (one guest access each):
 * MOV r,[m] / [m],r / [m],imm; MOVZX/MOVSX r,[m]; ALU r,[m]; CMP and TEST
 * with a memory operand; PUSH/POP r32. Read-modify-write, string, segment-
 * override and address-size forms still decline.
 */
#ifdef QEMU_XEMU_WASM_STATS_H
/*
 * Coverage diagnostics: "xdecl:<op>" counts each decline by the opcode
 * (0f xx for two-byte, with 66/prefix bytes kept) of the instruction that
 * stopped decoding, so the forms that keep whole groups out are visible.
 */
static void xwd_note_decline(const XwdCapture *c, unsigned start, unsigned end)
{
    char key[40];
    int n = snprintf(key, sizeof(key), "xdecl:");
    for (unsigned i = start; i < end && i < start + 3 && n < 36; i++) {
        n += snprintf(key + n, sizeof(key) - n, "%02x", c->bytes[i]);
        if (c->bytes[i] != 0x0f && c->bytes[i] != 0x66 &&
            !(c->bytes[i] >= 0x64 && c->bytes[i] <= 0x67) &&
            c->bytes[i] != 0xf2 && c->bytes[i] != 0xf3) {
            break;
        }
    }
    /*
     * Group opcodes: append the ModRM reg field (sub-operation) and whether
     * the operand is a register or memory. The opcode is the last byte
     * printed (after 66/0f/segment prefixes); its ModRM follows.
     */
    unsigned k = (n - 6) / 2, op = c->bytes[start + k - 1];
    bool two = k >= 2 && c->bytes[start + k - 2] == 0x0f;
    if (start + k < end &&
        (two || op == 0xff || op == 0xfe || op == 0x80 || op == 0x81 ||
         op == 0x83 || op == 0xc0 || op == 0xc1 || op == 0xd0 ||
         op == 0xd1 || op == 0xd3 || op == 0xf6 || op == 0xf7 ||
         (op <= 0x3b && (op & 7) <= 3))) {
        unsigned m = c->bytes[start + k];
        snprintf(key + n, sizeof(key) - n, "/%u%s", (m >> 3) & 7,
                 (m >> 6) == 3 ? "r" : "m");
    }
    xemu_wasm_count_add(g_intern_string(key), 1);
}
#endif

static bool xwd_decode_one(const XwdCapture *c, XwdPlan *p, bool allow_memory,
                           unsigned *fail_start, unsigned *fail_end);

static bool xwd_decode_ex(const XwdCapture *c, XwdPlan *p, bool allow_memory)
{
    unsigned fs = 0, fe = 0;
    bool ok = xwd_decode_one(c, p, allow_memory, &fs, &fe);
#ifdef QEMU_XEMU_WASM_STATS_H
    if (!ok && fe > fs) {
        xwd_note_decline(c, fs, fe);
    }
#endif
    return ok;
}

static bool xwd_decode_one(const XwdCapture *c, XwdPlan *p, bool allow_memory,
                           unsigned *fail_start, unsigned *fail_end)
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
        *fail_start = start;
        *fail_end = c->end[i];
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
            if (!xwd_modrm_ex(&r, &reg, &rm, o, allow_memory)) {
                return false;
            }
            o->op = XWD_MOV;
            if (!(b & 1)) {
                o->width = 8;
            }
            o->dst = b & 2 ? reg : rm;
            o->src = b & 2 ? rm : reg;
            o->mem_store = o->mem && !(b & 2);
        } else if (b == 0xc6 || b == 0xc7) {
            if (!xwd_modrm_ex(&r, &reg, &rm, o, allow_memory) || reg != 0) {
                return false;
            }
            o->mem_store = o->mem;
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
                if (!xwd_modrm_ex(&r, &reg, &rm, o, allow_memory)) {
                    return false;
                }
                /* [m] op= r: read-modify-write (CMP only reads) */
                o->mem_rmw = o->mem && !(b & 2) && o->op != XWD_CMP;
                o->dst = b & 2 ? reg : rm;
                o->src = b & 2 ? rm : reg;
            } else {
                o->immediate = true;
                o->imm = xwd_read(&r, o->width / 8);
            }
        } else if (b == 0x80 || b == 0x81 || b == 0x83) {
            if (!xwd_modrm_ex(&r, &reg, &rm, o, allow_memory) ||
                !xwd_alu(reg, o)) {
                return false;
            }
            o->mem_rmw = o->mem && o->op != XWD_CMP;
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
                if (!xwd_modrm_ex(&r, &reg, &rm, o, allow_memory)) {
                    return false;
                }
                o->dst = rm;
                o->src = reg;
            } else {
                o->immediate = true;
                o->imm = xwd_read(&r, o->width / 8);
            }
        } else if (b == 0xc0 || b == 0xc1 || b == 0xd0 || b == 0xd1) {
            /*
             * SHL/SAL, SHR, SAR r/m by imm8 or 1, register operand only.
             * The count is masked to 5 bits; a zero count changes nothing
             * (no flags, no write), exactly as the original translation.
             */
            if (!xwd_modrm(&r, &reg, &rm) || reg < 4) {
                return false;
            }
            o->op = reg == 5 ? XWD_SHR : reg == 7 ? XWD_SAR : XWD_SHL;
            o->dst = rm;
            if (!(b & 1)) {
                o->width = 8;
            }
            o->imm = (b <= 0xc1 ? xwd_read(&r, 1) : 1) & 31;
            if (!o->imm) {
                o->op = XWD_NOP;
            }
        } else if (b == 0xf6 || b == 0xf7) {
            if (!xwd_modrm_ex(&r, &reg, &rm, o, allow_memory) ||
                (o->mem && reg != 0 && reg != 2 && reg != 3)) {
                return false;
            }
            o->mem_rmw = o->mem && (reg == 2 || reg == 3);
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
                bool ext = b == 0xb6 || b == 0xb7 || b == 0xbe || b == 0xbf;
                if (!xwd_modrm_ex(&r, &reg, &rm, o, allow_memory && ext)) {
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
        } else if (b == 0xff && allow_memory && !prefix &&
                   r.pos < r.end && ((c->bytes[r.pos] >> 3) & 7) == 6 &&
                   (c->bytes[r.pos] >> 6) != 3) {
            /* PUSH r/m32 with a memory operand: two accesses */
            if (xwd_modrm_ex(&r, &reg, &rm, o, true) != 2) {
                return false;
            }
            o->op = XWD_PUSHM;
            o->mem_store = false;   /* the first access; the push follows */
        } else if (b == 0xfe || b == 0xff) {
            if (!xwd_modrm_ex(&r, &reg, &rm, o, allow_memory) || reg > 1) {
                return false;
            }
            o->mem_rmw = o->mem;
            o->op = reg ? XWD_DEC : XWD_INC;
            o->dst = rm;
            if (b == 0xfe) {
                o->width = 8;
            }
        } else if (b == 0xf8 || b == 0xf9 || b == 0xf5) {
            o->op = b == 0xf8 ? XWD_CLC : b == 0xf9 ? XWD_STC : XWD_CMC;
        } else if ((b == 0x6a || b == 0x68) && allow_memory && !prefix) {
            /* PUSH imm8 (sign-extended) / imm32 */
            o->op = XWD_PUSH;
            o->immediate = true;
            o->imm = b == 0x6a ? xwd_signed_byte(&r) : xwd_read(&r, 4);
            o->mem = o->mem_store = true;
            o->base = o->index = 0xff;
        } else if (b == 0xe8 && allow_memory && !prefix) {
            /* CALL rel32: push the next EIP, then jump (terminal) */
            o->op = XWD_CALL;
            o->imm = xwd_read(&r, 4);
            o->mem = o->mem_store = true;
            o->base = o->index = 0xff;
            p->delta = r.end + o->imm;
            terminal = true;
        } else if (b >= 0xa0 && b <= 0xa3 && allow_memory) {
            /* MOV AL/eAX, [moffs32] and MOV [moffs32], AL/eAX */
            o->op = XWD_MOV;
            if (!(b & 1)) {
                o->width = 8;
            }
            o->mem = true;
            o->mem_store = b >= 0xa2;
            o->base = o->index = 0xff;
            o->disp = xwd_read(&r, 4);
            o->dst = o->mem_store ? XWD_MEMREG : 0;
            o->src = o->mem_store ? 0 : XWD_MEMREG;
        } else if (b >= 0x50 && b <= 0x5f && allow_memory && !prefix) {
            /* PUSH r32 stores to [ESP-4]; POP r32 loads from [ESP] */
            o->op = b < 0x58 ? XWD_PUSH : XWD_POP;
            o->src = o->dst = b & 7;
            o->mem = true;
            o->mem_store = b < 0x58;
            o->base = o->index = 0xff;
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
        if (o->mem) {
            o->mem_width = o->op == XWD_MOVZX || o->op == XWD_MOVSX ?
                           o->src_width : o->width;
            p->accesses += o->op == XWD_PUSHM || o->mem_rmw ? 2 : 1;
        }
        p->needs_flags |= o->op >= XWD_ADC && o->op <= XWD_SETCC;
    }
    return start == c->size && (terminal || c->fallthrough);
}

static bool xwd_decode(const XwdCapture *c, XwdPlan *p)
{
    return xwd_decode_ex(c, p, false);
}

/* Internal function ABI: (WasmContext*) -> i32, with private i32 locals. */
enum {
    XD_ENV = 1, XD_REG = 2, XD_DST = 10, XD_SRC = 11,
    XD_LEFT = 12, XD_RIGHT = 13, XD_RESULT = 14, XD_TMP = 15,
    XD_SRC2 = 16, XD_FLAGS = 17, XD_DELTA = 18,
    /* Region-only architectural packet and selected edge. */
    XD_CC_OP = 19, XD_PC = 20, XD_TAKEN = 21,
    /* Memory stage: address, value, TLB entry and hit flag of an access. */
    XD_MADDR = 22, XD_MVAL = 23, XD_MENTRY = 24, XD_MHIT = 25,
    /* Read-modify-write: new lazy-CC operands, held until the store */
    XD_MSD = 26, XD_MSS = 27, XD_MSS2 = 28,
    /* First local index past the direct body's own (region-private go here). */
    XD_NLOCALS = 29,
};

struct XwdEmitter;

/*
 * One guest memory access of a direct body. The emitter fills insn/index/
 * store/width; resolve() fills the rest from the ORIGINAL translation of the
 * same instruction (never invented here): MemOpIdx, the softmmu fast-path
 * TLB layout, and integration data (helper, GETPC) in opaque.
 */
typedef struct XwdMemAccess {
    uint32_t insn, index;       /* instruction index, whole-TB access ordinal */
    bool store;
    uint8_t width;              /* 8, 16 or 32 */
    uint32_t oi;                /* original MemOpIdx */
    int32_t mask_ofs, table_ofs; /* env-relative CPUTLBDescFast mask/table */
    uint32_t cmp_ofs, addend_ofs; /* within CPUTLBEntry: addr_read/write */
    uint32_t index_shift;       /* TARGET_PAGE_BITS - CPU_TLB_ENTRY_BITS */
    uint32_t fold_shift, fold_mask; /* tlb index fold: a ^ ((a >> s) & m) */
    uint32_t page_mask;         /* TARGET_PAGE_MASK */
    uint32_t a_mask, s_mask;    /* alignment and span (size - 1) masks */
    void *opaque;
} XwdMemAccess;

/*
 * Integration callbacks (owned by the backend/region code). Locals are
 * unbiased XD_* indices: loads must leave the value in value_local, stores
 * read it there. resume_boundary is emitted after the fast path with an empty
 * operand stack; slow runs only on a TLB miss and performs the access through
 * the original helper (spills, snapshots, faults, unwind are its business);
 * shadow replaces the whole access in verify mode (xwmo_*, no guest memory).
 */
typedef struct XwdMemHooks {
    void *opaque;
    bool (*resolve)(struct XwdEmitter *e, unsigned insn, unsigned access,
                    bool store, unsigned width, XwdMemAccess *out);
    bool (*resume_boundary)(struct XwdEmitter *e, const XwdMemAccess *a);
    bool (*slow)(struct XwdEmitter *e, const XwdMemAccess *a,
                 unsigned addr_local, unsigned value_local);
    bool (*shadow)(struct XwdEmitter *e, const XwdMemAccess *a,
                   unsigned addr_local, unsigned value_local);
} XwdMemHooks;

typedef struct XwdEmitter {
    XwdCode *code;
    const XwdLayout *layout;
    uint8_t valid, dirty;
    bool flags_dirty, flags_cached, src2_dirty;
    bool resident;
    /* ctx stays at zero; private locals may be relocated. */
    unsigned local_bias;
    uint32_t cc, cc_helper;
    /* Memory stage: NULL declines any plan with accesses. */
    const XwdMemHooks *mem;
    unsigned access;            /* next whole-TB access ordinal */
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
    xwd_uleb(e, n ? n + e->local_bias : 0);
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
    if (reg == XWD_MEMREG) {
        /* the value this instruction's access loaded */
        xwd_local(e, 0x20, XD_MVAL);
        xwd_mask(e, width);
        return;
    }
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
    const unsigned local[] = { XD_DST, XD_SRC, XD_SRC2, XD_CC_OP };

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
            } else if (e->resident) {
                xwd_local(e, 0x20, local[n]);
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

#include "wasm32-direct-mem.c.inc"

/* No function prologue/epilogue: also used by the resident region bank. */
static bool xwd_emit_insns(XwdEmitter *out, const XwdPlan *p, bool verify)
{
    XwdEmitter e = *out;
    const XwdLayout *l = e.layout;

    if (p->version != XWD_VERSION || !p->count || p->count > XWD_MAX_INSNS ||
        (p->needs_flags && e.cc_helper == UINT32_MAX) ||
        (p->accesses && !e.mem)) {
        return false;
    }
    for (unsigned i = 0; i < p->count; i++) {
        const XwdInsn *o = &p->insn[i];
        unsigned wi = o->width == 8 ? 0 : o->width == 16 ? 1 : 2;
        if (!verify && (!i || i + 1 == p->count)) {
            xwd_io(&e, i + 1 == p->count);
        }
        if (o->mem) {
            /* the access first: nothing architectural changes before it */
            if (!xwd_mem_insn(&e, o, i, p, verify)) {
                return false;
            }
            if (o->op == XWD_PUSHM) {
                /* the loaded value goes to [ESP-4], then ESP commits */
                xwd_get(&e, 4, 32);
                xwd_const(&e, 4);
                xwd_byte(&e, 0x6b);
                xwd_local(&e, 0x21, XD_MADDR);
                if (!xwd_mem_access(&e, i, verify, true, 32)) {
                    return false;
                }
            }
            if (o->op == XWD_PUSH || o->op == XWD_CALL || o->op == XWD_PUSHM) {
                xwd_local(&e, 0x20, XD_MADDR);
                xwd_local(&e, 0x21, XD_RESULT);
                xwd_put(&e, 4, 32);
                continue;
            }
            if (o->op == XWD_POP) {
                /* ESP += 4, then the destination (POP ESP: the value) */
                xwd_get(&e, 4, 32);
                xwd_const(&e, 4);
                xwd_byte(&e, 0x6a);
                xwd_local(&e, 0x21, XD_RESULT);
                xwd_put(&e, 4, 32);
                xwd_local(&e, 0x20, XD_MVAL);
                xwd_local(&e, 0x21, XD_RESULT);
                xwd_put(&e, o->dst, 32);
                continue;
            }
            if (o->mem_store) {
                continue;       /* MOV [m], r/imm: the store was all */
            }
        }
        if (o->op == XWD_NOP || o->op == XWD_JMP) {
            continue;
        }
        if (o->op == XWD_JCC) {
            xwd_const(&e, p->delta);
            xwd_const(&e, p->fall_delta);
            xwd_condition(&e, o->cond);
            if (e.resident) {
                /* Equal target PCs do not make Jcc slots interchangeable. */
                xwd_local(&e, 0x22, XD_TAKEN);
            }
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
        } else if (o->op == XWD_SHL || o->op == XWD_SHR ||
                   o->op == XWD_SAR) {
            /*
             * As the original: T0 (sign-extended for SAR), cc_src =
             * T0 shifted by count - 1, cc_dst = result = T0 shifted by
             * count; CC_OP_SHL for SHL, CC_OP_SAR for SHR and SAR.
             */
            uint8_t sh = o->op == XWD_SHL ? 0x74 :
                         o->op == XWD_SHR ? 0x76 : 0x75;
            xwd_get(&e, o->dst, o->width);
            if (o->op == XWD_SAR && o->width != 32) {
                xwd_byte(&e, o->width == 8 ? 0xc0 : 0xc1);
            }
            xwd_local(&e, 0x22, XD_LEFT);
            xwd_const(&e, o->imm - 1);
            xwd_byte(&e, sh);
            xwd_local(&e, 0x21, XD_SRC);
            xwd_local(&e, 0x20, XD_LEFT);
            xwd_const(&e, o->imm);
            xwd_byte(&e, sh);
            xwd_local(&e, 0x22, XD_DST);
            e.cc = o->op == XWD_SHL ? l->cc_shl[wi] : l->cc_sar[wi];
            e.flags_dirty = true;
            e.flags_cached = false;
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
            /* read-modify-write: new CC operands wait in scratch (store) */
            const unsigned l_dst = o->mem_rmw ? XD_MSD : XD_DST,
                           l_src = o->mem_rmw ? XD_MSS : XD_SRC,
                           l_src2 = o->mem_rmw ? XD_MSS2 : XD_SRC2;
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
                        xwd_local(&e, 0x21, l_src2);
                        e.src2_dirty = true;
                    } else {
                        xwd_local(&e, 0x21, l_src);
                    }
                }
                if (o->op != XWD_NOT && o->op != XWD_INC && o->op != XWD_DEC) {
                    xwd_local(&e, 0x20, XD_RIGHT);
                    xwd_local(&e, 0x21, l_src);
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
                    xwd_local(&e, 0x20, l_src2);
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
                    xwd_local(&e, 0x22, l_dst);
                    e.flags_dirty = true;
                    e.flags_cached = false;
                }
            }
        }
        xwd_local(&e, 0x21, XD_RESULT);
        if (o->mem_rmw) {
            /* store the result to the same [m]; then the CC operands */
            xwd_local(&e, 0x20, XD_RESULT);
            xwd_local(&e, 0x21, XD_MVAL);
            if (!xwd_mem_access(&e, i, verify, true, o->mem_width)) {
                return false;
            }
            if (o->op != XWD_NOT) {
                xwd_local(&e, 0x20, XD_MSD);
                xwd_local(&e, 0x21, XD_DST);
                xwd_local(&e, 0x20, XD_MSS);
                xwd_local(&e, 0x21, XD_SRC);
                if (o->op == XWD_ADC || o->op == XWD_SBB) {
                    xwd_local(&e, 0x20, XD_MSS2);
                    xwd_local(&e, 0x21, XD_SRC2);
                }
            }
            continue;
        }
        if (o->op != XWD_CMP && o->op != XWD_TEST) {
            xwd_put(&e, o->dst, o->width);
        }
    }
    *out = e;
    return e.code->valid;
}

static bool xwd_emit_ex(const XwdPlan *p, const XwdLayout *l, bool verify,
                        uint32_t scratch, uint32_t cc_helper,
                        const XwdMemHooks *mem, XwdCode *code)
{
    XwdEmitter e = { .code = code, .layout = l, .cc_helper = cc_helper,
                     .mem = mem };

    memset(code, 0, sizeof(*code));
    if (p->version != XWD_VERSION || !p->count || p->count > XWD_MAX_INSNS ||
        (p->needs_flags && cc_helper == UINT32_MAX)) {
        return false;
    }
    code->valid = true;
    /* The body includes its local declaration and final end. */
    xwd_byte(&e, 1);
    xwd_byte(&e, p->accesses ? XD_NLOCALS - 1 : XD_DELTA);
    xwd_byte(&e, 0x7f);
    xwd_local(&e, 0x20, 0);
    xwd_mem(&e, 0x28, 0); /* WasmContext.env, unchanged external ABI */
    xwd_local(&e, 0x21, XD_ENV);
    if (!xwd_emit_insns(&e, p, verify)) {
        code->valid = false;
        return false;
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

static bool xwd_emit(const XwdPlan *p, const XwdLayout *l, bool verify,
                     uint32_t scratch, uint32_t cc_helper, XwdCode *code)
{
    return xwd_emit_ex(p, l, verify, scratch, cc_helper, NULL, code);
}
