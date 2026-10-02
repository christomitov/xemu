#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Execute the production Wasm exit-counter emitters in native WebAssembly.

Uses a host C compiler and Node. CC/NODE may contain command prefixes.
No emulator/game execution; the complete Emscripten build is a separate test.
"""
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SOURCE = (ROOT / "tcg/wasm32/tcg-target.c.inc").read_text()
DRIVER = (ROOT / "tcg/wasm32.c").read_text()
HEADER = (ROOT / "include/qemu/xemu-wasm-stats.h").read_text()
FIELDS = re.findall(r"\bX\((\w+)\)", HEADER)


def function(name, source=SOURCE):
    match = re.search(r"(?m)^(?:static )?[^\n;]*\b" + name +
                      r"\([^;]*?\)\n\{", source)
    assert match, name
    depth = 1
    pos = match.end()
    while depth:
        depth += (source[pos] == "{") - (source[pos] == "}")
        pos += 1
    return source[match.start():pos]


C = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define EMSCRIPTEN 1
#define TB_EXIT_MASK 3
#define TB_EXIT_IDX0 0
#define TB_EXIT_IDX1 1
#define TB_EXIT_REQUESTED 3
#define CTX_IDX 0
#define L32_0 1
#define WASM_CTX_TB_PTR_OFF 8
#define W_BLOCKTYPE_VOID 0x40
#define W_LOCAL_GET 0x20
#define W_I64_LOAD 0x29
#define W_I64_STORE 0x37
#define W_I64_ADD 0x7c
#define W_I32_STORE 0x36
#define W_ELSE 0x05
#define W_END 0x0b
#define W_RETURN 0x0f
#define xemu_wasm_stats (*(Stats *)(uintptr_t)1024)
typedef struct { uint8_t b[4096]; unsigned n; } TCGContext;
static void wasm8(TCGContext *s, unsigned v) { s->b[s->n++] = v; }
static void uleb(TCGContext *s, unsigned v) {
    do { unsigned b = v & 127; v >>= 7; wasm8(s, b | (v ? 128 : 0)); }
    while (v);
}
static void sleb(TCGContext *s, int64_t v) {
    for (;;) {
        unsigned b = v & 127;
        v >>= 7;
        bool more = !((v == 0 && !(b & 64)) || (v == -1 && (b & 64)));
        wasm8(s, b | (more ? 128 : 0));
        if (!more) { break; }
    }
}
static void wasm_var(TCGContext *s, unsigned op, unsigned idx) {
    wasm8(s, op); uleb(s, idx);
}
static void wasm_i32_const(TCGContext *s, int32_t v) {
    wasm8(s, 0x41); sleb(s, v);
}
static void wasm_i64_const(TCGContext *s, int64_t v) {
    wasm8(s, 0x42); sleb(s, v);
}
static void wasm_mem(TCGContext *s, unsigned op, unsigned off) {
    wasm8(s, op); uleb(s, op == W_I32_STORE ? 2 : 3); uleb(s, off);
}
static void wasm_if(TCGContext *s, unsigned ty) { wasm8(s, 4); wasm8(s, ty); }
"""
C += "typedef struct {\n" + "\n".join(
    "uint64_t " + name + ";" for name in FIELDS) + "\n} Stats;\n"
for name in ["wasm_ctx_store_i32_const", "wasm_count", "wasm_profile_exits",
             "wasm_count_exit_tb", "wasm_count_goto_exit", "wasm_exit_tb"]:
    C += function(name) + "\n"
for name in ["wasm32_ic_mode", "wasm32_ic_enabled", "wasm32_ic_for_exit"]:
    C += function(name, DRIVER) + "\n"
C += r"""
int main(int argc, char **argv) {
    assert(argc == 2 || argc == 3);
    setenv("XEMU_WASM_JIT_ENTRY_PROF", argv[1], 1);
    const char *mode = argc == 3 ? argv[2] : "unset";
    if (strcmp(mode, "unset")) { setenv("XEMU_WASM_IC", mode, 1); }
    else { unsetenv("XEMU_WASM_IC"); }
    bool all = mode[0] == '1', indirect = mode[0] != '0';
    assert(wasm32_ic_enabled() == indirect);
    assert(wasm32_ic_for_exit(false) == all);
    assert(wasm32_ic_for_exit(true) == indirect);
    const unsigned args[] = {0, 0x100, 0x101, 0x103, 0x102};
    for (unsigned i = 0; i < 7; i++) {
        TCGContext s = {0};
        if (i < 5) {
            wasm_exit_tb(&s, args[i]);
        } else {
            wasm_count_goto_exit(&s, i == 6);
            wasm_i32_const(&s, 42);
            wasm8(&s, W_RETURN);
        }
        for (unsigned j = 0; j < s.n; j++) { printf("%02x", s.b[j]); }
        puts("");
    }
}
"""


def uleb(value):
    result = bytearray()
    while True:
        byte = value & 127
        value >>= 7
        result.append(byte | (128 if value else 0))
        if not value:
            return bytes(result)


def string(text):
    data = text.encode()
    return uleb(len(data)) + data


def section(kind, data):
    return bytes([kind]) + uleb(len(data)) + data


def module(bodies):
    # All fixtures are (ctx:i32, target:i32)->i32, sharing one test memory.
    data = b"\0asm\x01\0\0\0" + section(1, b"\x01\x60\x02\x7f\x7f\x01\x7f")
    data += section(2, b"\x01" + string("env") + string("mem") +
                    b"\x02\x00\x01")
    data += section(3, uleb(len(bodies)) + bytes(len(bodies)))
    exports = uleb(len(bodies))
    code = uleb(len(bodies))
    for i, body in enumerate(bodies):
        exports += string("f" + str(i)) + b"\x00" + uleb(i)
        body = b"\x00" + body + b"\x0b"
        code += uleb(len(body)) + body
    return data + section(7, exports) + section(10, code)


JS = r"""
const fs = require('fs'), assert = require('assert');
const fields = FIELDS;
const mem = new WebAssembly.Memory({initial: 1});
const data = new DataView(mem.buffer);
const {exports: e} = new WebAssembly.Instance(
  new WebAssembly.Module(fs.readFileSync(process.argv[2])), {env: {mem}});
const enabled = Number(process.argv[3]);
const names = ['plain', 'chain0', 'chain1', 'requested', 'other'];
const expected = new Map();
function check(name) {
  const key = 'n_jit_exit_' + name;
  expected.set(key, (expected.get(key) || 0) + enabled);
  fields.forEach((field, i) => assert.equal(
    data.getBigUint64(1024 + 8 * i, true),
    BigInt(expected.get(field) || 0), field));
}
for (let round = 0; round < 100; round++) {
  [0, 0x100, 0x101, 0x103, 0x102].forEach((arg, i) => {
    data.setUint32(128 + 8, 0x5555, true);
    assert.equal(e['f' + i](128, 0), arg);
    assert.equal(data.getUint32(128 + 8, true), 0);
    check(names[i]);
  });
  assert.equal(e.f5(128, 1234), 42); check('direct');
  assert.equal(e.f6(128, 1234), 42); check('ptr');
  assert.equal(e.f6(128, 0), 42); check('ptr_null');
}
console.log('PASS: native Wasm exit kinds, return/context, profile=' + enabled);
""".replace("FIELDS", json.dumps(FIELDS))

with tempfile.TemporaryDirectory(prefix="test-wasm-exits-") as directory:
    p = Path(directory)
    (p / "emit.c").write_text(C)
    (p / "test.cjs").write_text(JS)
    subprocess.run(shlex.split(os.environ.get("CC", "cc")) +
                   ["-std=gnu11", "-O2", str(p / "emit.c"),
                    "-o", str(p / "emit")],
                   check=True, timeout=30)
    for mode in ["unset", "", "0", "1", "2", "3", "invalid"]:
        subprocess.run([str(p / "emit"), "0", mode], check=True,
                       stdout=subprocess.DEVNULL, timeout=10)
    print("PASS: IC default/off/all/indirect-only/invalid mode policies")
    for enabled in [0, 1]:
        lines = subprocess.check_output([str(p / "emit"), str(enabled)],
                                        text=True, timeout=10).splitlines()
        bodies = [bytes.fromhex(line) for line in lines]
        if not enabled:
            assert bodies[5] == bodies[6] == b"\x41\x2a\x0f", "default overhead"
        (p / "test.wasm").write_bytes(module(bodies))
        subprocess.run(shlex.split(os.environ.get("NODE", "node")) +
                       [str(p / "test.cjs"), str(p / "test.wasm"),
                        str(enabled)],
                       check=True, timeout=30)
