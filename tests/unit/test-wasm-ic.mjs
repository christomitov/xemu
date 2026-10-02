// SPDX-License-Identifier: GPL-2.0-or-later
// Run from any directory: node tests/unit/test-wasm-ic.mjs (Node 20+).
// Exercise the actual EM_JS cache manager against real cross-instance Wasm
// calls; no emulator, graphics, or guest-image allocation. This tests cache
// lifetime/dispatch, not the TCG emitter or Asyncify (use the intro stress
// run for those).
import fs from 'node:fs';
import assert from 'node:assert/strict';
const text = fs.readFileSync(
  new URL('../../tcg/wasm32.c', import.meta.url), 'utf8');
function body(name) {
  const start = text.indexOf(`EM_JS(void, ${name},`);
  assert(start >= 0);
  const open = text.indexOf('{', start);
  return text.slice(open + 1, text.indexOf('\n});', open));
}
const free = [];
let next = 1;
globalThis.Module = {};
globalThis.HEAP32 = new Int32Array(new SharedArrayBuffer(4));
globalThis.wasmMemory = new WebAssembly.Memory({ initial: 1 });
globalThis.wasmTable = new WebAssembly.Table({
  element: 'anyfunc', initial: 256,
});
globalThis.addFunction = fn => {
  const f = free.length ? free.pop() : next++;
  assert(f < 256);
  assert.equal(wasmTable.get(f), null);
  wasmTable.set(f, fn);
  return f;
};
globalThis.removeFunction = f => {
  assert.notEqual(wasmTable.get(f), null);
  wasmTable.set(f, null);
  free.push(f);
};
new Function('collected', body('wasm32_js_init'))(0);
const fill = new Function('source', 'slot', 'header', 'target',
                          body('wasm32_ic_fill'));
const J = Module.__wasm32_jit;
const leb = x => {
  const b = [];
  do { const a = x & 127; x >>>= 7; b.push(a | (x ? 128 : 0)); } while (x);
  return b;
};
const str = s => [...leb(s.length), ...Buffer.from(s)];
const sec = (id, b) => [id, ...leb(b.length), ...b];
const exp = (s, kind, n) => [...str(s), kind, ...leb(n)];
// start(key): tail-call slot 0 on a matching header, otherwise return self ID.
// Two cache slots let us test the manager's per-site separation too.
const code = [0, 0x20, 0, 0x23, 1, 0x46, 0x04, 0x7f,
              0x20, 0, 0x41, 0, 0x13, 0, 0, 0x05, 0x23, 0, 0x0b, 0x0b];
const mod = new WebAssembly.Module(new Uint8Array([
  0, 97, 115, 109, 1, 0, 0, 0,
  ...sec(1, [1, 0x60, 1, 0x7f, 1, 0x7f]),
  ...sec(2, [1, ...str('env'), ...str('ic_table'), 1, 0x70, 1, 2, 2]),
  ...sec(3, [1, 0]),
  ...sec(6, [3, 0x7f, 1, 0x41, 0, 0x0b,
               0x7f, 1, 0x41, 0x7f, 0x0b, 0x7f, 1, 0x41, 0x7f, 0x0b]),
  ...sec(7, [4, ...exp('start', 0, 0), ...exp('ic_self', 3, 0),
            ...exp('ic0', 3, 1), ...exp('ic1', 3, 2)]),
  ...sec(10, [1, ...leb(code.length), ...code]),
]));
const install = () => J.install(mod, {}, 0, 0, 2);
function check() {
  let links = 0, reverse = 0;
  for (const [f, r] of J.mods) {
    assert.notEqual(wasmTable.get(f), null);
    for (const link of r.links) {
      if (link.target) {
        links++;
        assert(J.mods.has(link.target));
        assert(J.incoming.get(link.target).has(link));
        assert.equal(link.table.get(link.slot), wasmTable.get(link.target));
        assert.notEqual(link.header.value, -1);
      } else {
        assert.equal(link.header.value, -1);
        assert.equal(link.table.get(link.slot), null);
      }
    }
  }
  for (const [f, set] of J.incoming) {
    assert(J.mods.has(f));
    assert(set.size > 0);
    for (const link of set) { assert.equal(link.target, f); reverse++; }
  }
  assert.equal(links, reverse);
}
const a = install(), b = install(), c = install();
assert.equal(wasmTable.get(a)(100), a);
fill(a, 0, 100, b);
assert.equal(wasmTable.get(a)(100), b); // actual return_call_indirect
assert.equal(wasmTable.get(a)(101), a); // mismatched validated header
fill(a, 0, 0x81234000, b);
assert.equal(wasmTable.get(a)(0x81234000), b); // unsigned pointer/i32 ABI
fill(a, 0, 100, b);
fill(a, 1, 200, c);
fill(c, 0, 100, b);
check();
J.remove(b);
check();
assert.equal(wasmTable.get(a)(100), a); // incoming guard cleared
const reused = install();
assert.equal(reused, b);                // function-table ABA
assert.equal(wasmTable.get(a)(100), a); // old cache must not call new b
fill(a, 0, 104, reused);
assert.equal(wasmTable.get(a)(104), reused);
fill(reused, 0, 104, a);                // create a retention cycle
check();
J.remove(a);                           // clears incoming AND outgoing edges
check();
assert.equal(wasmTable.get(reused)(104), reused);
let seed = 0x71c;
const random = n => {
  seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0;
  return seed % n;
};
for (let i = 0; i < 10000; i++) {
  const keys = [...J.mods.keys()];
  const op = random(10);
  if (!keys.length || (op < 2 && keys.length < 64)) {
    install();
  } else if (op < 4) {
    J.remove(keys[random(keys.length)]);
  } else {
    const src = keys[random(keys.length)], dst = keys[random(keys.length)];
    fill(src, random(2), (i + 1000) * 4, dst);
  }
  check();
}
for (const f of [...J.mods.keys()]) J.remove(f);
check();
assert.equal(J.incoming.size, 0);
assert.equal(J.mods.size, 0);
for (let f = 1; f < next; f++) assert.equal(wasmTable.get(f), null);
console.log('PASS: native cross-instance tail call; mismatch, index reuse, ' +
            'cycles, 10000 lifecycle mutations, empty final graph');
