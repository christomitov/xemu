#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Actual-source direct-region blueprint codec and bounded module scanner.

Only value ownership, framing and independent instruction decoding are tested.
Synthetic function-table indices are never called. No production group builder,
live-link/IRQ contract, emulator execution or performance claim is made here.
"""
import importlib.util
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

spec = importlib.util.spec_from_file_location(
    'direct', Path(__file__).with_name('test-wasm-direct.py'))
direct = importlib.util.module_from_spec(spec)
spec.loader.exec_module(direct)
ROOT = direct.ROOT

C = direct.C[:direct.C.index('int main(void)')] + r'''
#include "tcg/wasm32-direct-blueprint.c.inc"
static unsigned probes;
static const unsigned fixed_words = 44;
static uint32_t seed = 0x86c0de32;
static uint32_t random_word(void) {
    seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
    return seed;
}
static void word(uint8_t *p, uint32_t x) {
    for (unsigned i = 0; i < 4; i++) p[i] = x >> (8 * i);
}
static void reject(const uint8_t *p, unsigned n, bool module) {
    XwdTranslation t; XwdPlan plan;
    memset(&t, 0xa5, sizeof(t)); memset(&plan, 0xa5, sizeof(plan));
    assert(!(module ? xwd_blueprint_find(p, n, &t, &plan) :
                      xwd_blueprint_read(p, n, &t, &plan)));
    assert(!t.capture.valid && !plan.version);
    probes++;
}
static void same(const XwdTranslation *t, const XwdCode *want) {
    XwdCode code;
    assert(xwd_blueprint_write(t, &code) && code.valid);
    assert(code.size == want->size);
    assert(!memcmp(code.bytes, want->bytes, code.size));
    probes++;
}
static void roundtrip(const XwdTranslation *t, XwdCode *code) {
    XwdTranslation got; XwdPlan actual, want;
    assert(xwd_blueprint_write(t, code));
    assert(code->size == (fixed_words + t->capture.count) * 4 +
                        t->capture.size);
    assert(xwd_blueprint_read(code->bytes, code->size, &got, &actual));
    assert(xwd_decode(&t->capture, &want));
    /* Plans are initialized by the real decoder on both paths. */
    assert(!memcmp(&actual, &want, sizeof(want)));
    assert(got.layout.can_do_io == t->layout.can_do_io);
    assert(got.layout.icount_decr == t->layout.icount_decr);
    same(&got, code);
    uint8_t *copy = malloc(code->size); assert(copy);
    memcpy(copy, code->bytes, code->size);
    assert(xwd_blueprint_read(copy, code->size, &got, &actual));
    memset(copy, 0xa5, code->size); free(copy);
    same(&got, code); /* No borrowed capture/layout backing. */
    for (unsigned n = 0; n < code->size; n++) reject(code->bytes, n, false);
}
static XwdTranslation translation(unsigned count, unsigned kind) {
    static const uint8_t insns[][8] = {
        {0x90}, {0x05, 0x12, 0x34, 0x56, 0x78}, {0xf5},
        {0x66, 0x8d, 0x84, 0x8d, 0x78, 0x56, 0x34, 0x12}
    };
    static const unsigned sizes[] = {1, 5, 1, 8};
    XwdTranslation t = {0};
    XwdCapture *c = &t.capture;
    t.layout = layout;
    t.layout.verify = (void *)(uintptr_t)17;
    t.layout.cc_all = (void *)(uintptr_t)23;
    t.layout.icount_decr = -8;
    c->version = XWD_VERSION; c->valid = true;
    c->pc = 0xfffffff0u + count; c->fallthrough = true;
    c->pcrel = count & 1; c->count = count;
    for (unsigned i = 0; i < count; i++) {
        memcpy(c->bytes + c->size, insns[kind], sizes[kind]);
        c->size += sizes[kind]; c->end[i] = c->size;
    }
    c->jump_valid = 1; c->jump_pc[0] = c->pc + c->size;
    return t;
}
static void invalid_write(XwdTranslation t) {
    XwdCode code; memset(&code, 0xa5, sizeof(code));
    assert(!xwd_blueprint_write(&t, &code));
    assert(!code.valid && !code.size); probes++;
}
static void leb(XwdEmitter *e, unsigned v, bool padded) {
    if (!padded) { xwd_uleb(e, v); return; }
    for (unsigned i = 0; i < 5; i++) {
        xwd_byte(e, ((v >> (i * 7)) & 127) | (i < 4 ? 128 : 0));
    }
}
static void module_init(XwdCode *code) {
    const uint8_t header[] = {0, 'a', 's', 'm', 1, 0, 0, 0};
    memset(code, 0, sizeof(*code)); code->valid = true;
    memcpy(code->bytes, header, sizeof(header)); code->size = sizeof(header);
}
static void section(XwdCode *m, const char *name, const XwdCode *data,
                    bool padded) {
    XwdEmitter e = {.code = m}; unsigned n = strlen(name);
    assert(n < 128);
    xwd_byte(&e, 0); leb(&e, n + (padded ? 5 : 1) + data->size, padded);
    leb(&e, n, padded);
    for (unsigned i = 0; i < n; i++) xwd_byte(&e, name[i]);
    for (unsigned i = 0; i < data->size; i++) xwd_byte(&e, data->bytes[i]);
    assert(m->valid);
}
int main(void) {
    check_capture();
    XwdCode code, bad, module, changed;
    XwdTranslation t, got; XwdPlan plan;
    for (unsigned n = 1; n <= XWD_MAX_INSNS; n++) {
        for (unsigned k = 0; k < 4; k++) {
            t = translation(n, k); roundtrip(&t, &code);
        }
    }
    assert(code.size == 944); /* Exact512-byte/64-instruction capture. */
    t = translation(1, 0);
    t.capture.bytes[0] = 0x74; t.capture.bytes[1] = 0;
    t.capture.size = t.capture.end[0] = 2; t.capture.fallthrough = false;
    t.capture.jump_valid = 3;
    t.capture.jump_pc[0] = t.capture.jump_pc[1] = t.capture.pc + 2;
    roundtrip(&t, &code); /* Equal PCs do not erase original slot metadata. */
    assert(xwd_blueprint_read(code.bytes, code.size, &got, &plan));
    assert(got.capture.jump_valid == 3 && plan.conditional);
    t = translation(2, 1); roundtrip(&t, &code);
    /* Independent LE field vector: no native layout/padding assumptions. */
    const uint32_t expected[] = {
        XWD_BLUEPRINT_VERSION, XWD_VERSION, t.capture.pc, 10, 2, 1, 1,
        t.capture.pc + 10, 0, 17, 23, 0, 32, 40, 44, 48, 52,
        0xfffffffcu, 0xfffffff8u, CC_OP_EFLAGS, 9000, 0x1111, 0x2222,
        CC_OP_ADDB, CC_OP_ADDW, CC_OP_ADDL,
        CC_OP_SUBB, CC_OP_SUBW, CC_OP_SUBL,
        CC_OP_LOGICB, CC_OP_LOGICW, CC_OP_LOGICL,
        CC_OP_ADCB, CC_OP_ADCW, CC_OP_ADCL,
        CC_OP_SBBB, CC_OP_SBBW, CC_OP_SBBL,
        CC_OP_INCB, CC_OP_INCW, CC_OP_INCL,
        CC_OP_DECB, CC_OP_DECW, CC_OP_DECL, 5, 10
    };
    assert(sizeof(expected) == (fixed_words + 2) * 4);
    for (unsigned i = 0; i < sizeof(expected) / sizeof(*expected); i++) {
        uint8_t want[4]; word(want, expected[i]);
        assert(!memcmp(code.bytes + i * 4, want, 4));
    }
    const struct { unsigned index; uint32_t value; } invalid[] = {
        {0, 0}, {0, XWD_BLUEPRINT_VERSION + 1}, {1, 0},
        {1, XWD_VERSION + 1},
        {3, 0}, {3, 513}, {3, UINT32_MAX}, {4, 0}, {4, 65},
        {4, UINT32_MAX}, {5, 4}, {5, UINT32_MAX}, {6, 4},
        {6, UINT32_MAX}, {9, 0}, {10, 0}, {44, 0}, {44, 11},
        {45, 5}, {45, 9}, {45, UINT32_MAX}
    };
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(*invalid); i++) {
        bad = code; word(bad.bytes + invalid[i].index * 4, invalid[i].value);
        reject(bad.bytes, bad.size, false);
    }
    bad = code; bad.bytes[(fixed_words + 2) * 4] = 0xf4;
    reject(bad.bytes, bad.size, false); /* Independently reject HLT. */
    bad = code; bad.bytes[bad.size++] = 0;
    reject(bad.bytes, bad.size, false);
    reject(NULL, 0, false); reject(NULL, 32, false);
    reject(code.bytes, XWD_MAX_CODE + 1, false);
#define BAD(field, value) do { XwdTranslation x = t; x.field = value; \
                               invalid_write(x); } while (0)
    BAD(capture.valid, false); BAD(capture.version, 0);
    BAD(capture.size, 0); BAD(capture.size, 513);
    BAD(capture.count, 0); BAD(capture.count, 65);
    BAD(capture.end[0], 0); BAD(capture.jump_valid, 4);
    BAD(layout.verify, NULL); BAD(layout.cc_all, NULL);
#if UINTPTR_MAX > UINT32_MAX
    BAD(layout.verify, (void *)((uintptr_t)UINT32_MAX + 1));
    BAD(layout.cc_all, (void *)((uintptr_t)UINT32_MAX + 1));
#endif
#undef BAD
    /* Exact names, padding, skipped ordinary/custom sections and ownership. */
    for (unsigned padded = 0; padded < 2; padded++) {
        module_init(&module);
        XwdEmitter e = {.code = &module};
        xwd_byte(&e, 1); xwd_byte(&e, 1); xwd_byte(&e, 0); /* empty types */
        section(&module, "other", &code, padded);
        section(&module, XWD_BLUEPRINT_NAME, &code, padded);
        for (unsigned n = 0; n < module.size; n++) {
            reject(module.bytes, n, true); /* Target section is last. */
        }
        assert(xwd_blueprint_find(module.bytes, module.size, &got, &plan));
        same(&got, &code);
        changed = module;
        section(&changed, "", &code, padded);
        assert(xwd_blueprint_find(changed.bytes, changed.size, &got, &plan));
        memset(changed.bytes, 0xa5, changed.size); same(&got, &code);
        changed = module; section(&changed, XWD_BLUEPRINT_NAME, &code, padded);
        reject(changed.bytes, changed.size, true);
        changed = module; changed.bytes[changed.size++] = 0;
        /* Truncated section length. */
        reject(changed.bytes, changed.size, true);
        changed = module; changed.bytes[changed.size++] = 0;
        changed.bytes[changed.size++] = 0;
        reject(changed.bytes, changed.size, true); /* Missing custom name. */
    }
    reject(NULL, 32, true);
    module_init(&changed); reject(changed.bytes, changed.size, true);
    section(&changed, XWD_BLUEPRINT_NAME "-other", &code, false);
    reject(changed.bytes, changed.size, true);
    changed = module; changed.bytes[0] = 1;
    reject(changed.bytes, changed.size, true);
    changed = module; changed.bytes[4] = 2;
    reject(changed.bytes, changed.size, true);
    /* Bad/overflowing/padded ULEBs at either framing layer. */
    const uint8_t malformed[][6] = {
        {0x80, 0x80, 0x80, 0x80, 0x10},
        {0x80, 0x80, 0x80, 0x80, 0x80, 0},
        {0xff, 0xff, 0xff, 0xff, 0x0f}
    };
    for (unsigned k = 0; k < 3; k++) {
        for (unsigned name = 0; name < 2; name++) {
            module_init(&changed); changed.bytes[changed.size++] = 0;
            if (name) changed.bytes[changed.size++] = 6;
            memcpy(changed.bytes + changed.size, malformed[k], 6);
            changed.size += 6; reject(changed.bytes, changed.size, true);
        }
    }
    /* Random malformed copies: successful parses must reserialize/redecode.
     * Layout words remain opaque values, not trusted execution permissions. */
    for (unsigned k = 0; k < 20000; k++) {
        bool scan = k & 1;
        changed = scan ? module : code;
        for (unsigned n = 1 + (random_word() & 3); n; n--) {
            changed.bytes[random_word() % changed.size] ^= random_word();
        }
        if (k & 2) changed.size = random_word() % (changed.size + 1);
        bool ok = scan ? xwd_blueprint_find(changed.bytes, changed.size,
                                           &got, &plan) :
                         xwd_blueprint_read(changed.bytes, changed.size,
                                           &got, &plan);
        if (ok) {
            XwdTranslation again; XwdPlan second;
            assert(xwd_blueprint_write(&got, &bad));
            assert(xwd_blueprint_read(bad.bytes, bad.size, &again, &second));
            assert(!memcmp(&plan, &second, sizeof(plan)));
        } else {
            assert(!got.capture.valid && !plan.version);
        }
        probes++;
    }
    printf("PASS: %u blueprint value/framing/decoder probes "
           "(not region routing)\n",
           probes);
}
'''


def main():
    with tempfile.TemporaryDirectory(prefix='direct-blueprint-') as tmp:
        tmp = Path(tmp)
        source, exe = tmp / 'test.c', tmp / 'test'
        source.write_text(C)
        subprocess.run([*shlex.split(os.environ.get('CC', 'cc')), '-std=gnu11',
                        '-O2', '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-function', '-DEMSCRIPTEN=1',
                        '-DCONFIG_TCG_WASM_JIT=1', '-I' + str(ROOT),
                        '-I' + str(ROOT / 'include'), str(source),
                        '-o', str(exe)], check=True)
        env = {k: v for k, v in os.environ.items() if not k.startswith('XEMU_')}
        env.update(XEMU_WASM_DIRECT_X86='2', XEMU_WASM_DIRECT_FLAGS='1')
        subprocess.run([str(exe)], env=env, check=True)


if __name__ == '__main__':
    main()
