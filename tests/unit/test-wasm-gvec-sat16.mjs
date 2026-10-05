// SPDX-License-Identifier: GPL-2.0-or-later
// Driver for test-wasm-gvec-sat16.py; original C helpers are the oracle.
import fs from 'node:fs';
import assert from 'node:assert/strict';
const dir = process.argv[2];
const cases = JSON.parse(fs.readFileSync(`${dir}/cases.json`));
const oracleCode = new WebAssembly.Module(fs.readFileSync(`${dir}/oracle.wasm`));
const shimCode = new WebAssembly.Module(fs.readFileSync(`${dir}/shim.wasm`));
const ctx = 256, stack = 8192, base = 16368, span = 192;
const liveValue = 0x123456789abcdefn, poison = 0xdeadbeefn;
let probes = 0, rewinds = 0, fast = 0, fallback = 0;
const deepDone = new Set();
function sizes(desc) {
  const max = ((desc & 255) + 1) * 8, f = (desc >>> 8) & 3;
  return [f === 2 ? max : (f + 1) * 8, max];
}
for (const c of cases) {
  const memory = new WebAssembly.Memory({initial: 2});
  const reference = new WebAssembly.Memory({initial: 2});
  const bytes = new Uint8Array(memory.buffer), ref = new Uint8Array(reference.buffer);
  const v = new DataView(memory.buffer), rv = new DataView(reference.buffer);
  const original = new WebAssembly.Instance(oracleCode, {env: {memory}}).exports;
  const expected = new WebAssembly.Instance(oracleCode, {env: {memory: reference}}).exports;
  const fn = `helper_${c.op}`;
  let calls = 0, completed = 0, yields = 0, async = false, generated, shim;
  shim = new WebAssembly.Instance(shimCode, {env: {
    memory, tb: p => generated.exports.run(p),
    oracle: (d, a, b, desc) => { completed++; original[fn](d, a, b, desc); },
    suspend: () => {
      const state = shim.exports.asyncify_get_state();
      if (state === 2) {
        shim.exports.asyncify_stop_rewind(); v.setInt32(64, 0, true);
      } else {
        assert.equal(state, 0); yields++;
        v.setInt32(64, 1, true); shim.exports.asyncify_start_unwind(96);
      }
    },
  }});
  const unexpected = () => { throw Error('unexpected chain/check import'); };
  generated = new WebAssembly.Instance(new WebAssembly.Module(fs.readFileSync(c.file)), {
    env: {memory, check: unexpected, s0: unexpected, s1: unexpected, chain: unexpected,
      slow: (d, a, b, desc) => {
        calls++;
        if (async) shim.exports.helper(d, a, b, desc);
        else original[fn](d, a, b, desc);
      },
    },
  });
  function prepare(d, a, b, desc) {
    for (const [i, value] of [d, a, b, desc].entries()) {
      v.setUint32(stack + i * 8, value >>> 0, true);
      v.setUint32(stack + i * 8 + 4, 0xcafebabe, true);
    }
    v.setUint32(ctx + 16, 1, true); v.setUint32(ctx + 24, 0, true);
    v.setUint32(64, 0, true);
    generated.exports.r4.value = poison;
  }
  function compare(lo, hi, detail) {
    for (let p = lo; p < hi; p++) {
      if (bytes[p] !== ref[p]) throw Error(`byte ${p}: ${bytes[p]} != ${ref[p]} ${detail}`);
    }
  }
  function probe(d, a, b, desc, lo = base, hi = base + span) {
    ref.set(bytes.subarray(lo, hi), lo);
    prepare(d, a, b, desc);
    const before = calls, [n, max] = sizes(desc);
    const accept = c.enabled && n === max && (n === 8 || n === 16) &&
                   !((d | a | b) & (n - 1));
    expected[fn](d, a, b, desc);
    assert.equal(generated.exports.run(ctx), 1);
    assert.equal(calls - before, accept ? 0 : 1);
    assert.equal(v.getBigUint64(ctx + 40, true), liveValue);
    if (!accept) {
      assert.equal(generated.exports.sp.value, BigInt(stack));
      assert.equal(generated.exports.r4.value, c.live ? poison : 0x44446666n);
      assert.equal(v.getUint32(ctx + 24, true), c.ic ? 77 : 0);
    }
    compare(lo, hi, `${c.op} ${d}/${a}/${b} desc=${desc}`);
    probes++; accept ? fast++ : fallback++;
  }
  // All low descriptor bits, including noncanonical encodings and tail clears.
  // Restrict maximum to 64 bytes here so every access fits the test arena.
  for (let maxCode = 0; maxCode < 8; maxCode++) {
    for (let opCode = 0; opCode < 4; opCode++) {
      for (const data of [0, 0x55555400, 0xfffffc00]) {
        for (const [d, a, b] of [[16416,16384,16480], [16384,16384,16480],
                                [16480,16384,16480], [16384,16384,16384],
                                [16386,16384,16480], [16384,16386,16480],
                                [16416,16386,16482]]) {
          for (let p = base; p < base + span; p++) bytes[p] = (p * 37 + maxCode * 11) & 255;
          probe(d, a, b, (data | maxCode | (opCode << 8)) >>> 0);
        }
      }
    }
  }
  // Larger valid max sizes, through the maximum descriptor value, stay slow.
  for (const maxCode of [15, 31, 255]) {
    for (let opCode = 0; opCode < 4; opCode++) {
      for (let p = base; p < base + 4096; p++) bytes[p] = (p * 37) & 255;
      probe(16416, 16384, 16480, maxCode | (opCode << 8), base, base + 4096);
    }
  }
  // Exact-width accesses at the memory limit: a 16-byte MMX load/store traps.
  for (const n of [8, 16]) {
    const end = bytes.length - n, desc = 512 | (n / 8 - 1);
    for (const role of ['a', 'b', 'd', 'all']) {
      bytes.fill(0x93, bytes.length - 64);
      bytes.fill(0x52, base, base + span); ref.set(bytes);
      const d = role === 'd' || role === 'all' ? end : 16416;
      const a = role === 'a' || role === 'all' ? end : 16384;
      const b = role === 'b' || role === 'all' ? end : 16480;
      probe(d, a, b, desc, base, bytes.length);
    }
  }
  // Sweep every signed 16-bit a against six b values, with widths/aliases mixed.
  if (c.enabled && c.live && c.ic && !deepDone.has(c.op)) {
    deepDone.add(c.op);
    for (let a0 = -32768; a0 <= 32767; a0++) {
      for (const b0 of [-32768, -12345, -1, 0, 1, 32767]) {
        const n = a0 & 1 ? 8 : 16, desc = 512 | (n / 8 - 1);
        const alias = (a0 >>> 1) & 3;
        const a = 16384, b = alias === 3 ? a : 16480;
        const d = alias === 1 || alias === 3 ? a : alias === 2 ? b : 16416;
        bytes.fill(0xa5, base, base + span);
        for (let lane = 0; lane < n / 2; lane++) {
          v.setInt16(a + lane * 2, a0 + lane * 997, true);
          v.setInt16(b + lane * 2, b0 - lane * 173, true);
        }
        probe(d, a, b, desc);
      }
    }
  }
  // Real Asyncify unwind + re-yield across main -> generated TB -> main helper.
  bytes.fill(0x73, base, base + span); ref.set(bytes);
  const d = 16416, a = 16384, b = 16480, desc = 1; // oprsz8, maxsz16: fallback
  expected[fn](d, a, b, desc); prepare(d, a, b, desc);
  v.setUint32(96, 110000, true); v.setUint32(100, 120000, true);
  const before = calls; async = true;
  let result = shim.exports.run(ctx);
  while (shim.exports.asyncify_get_state() === 1) {
    assert.equal(result, 0); assert.equal(yields <= 2, true);
    // A replay must skip the fresh fast attempt, even if its guard now matches.
    // The instrumented helper restores its original desc from its own frame.
    v.setUint32(stack + 24, 512, true);
    shim.exports.asyncify_stop_unwind();
    shim.exports.asyncify_start_rewind(96); v.setInt32(64, 2, true);
    result = shim.exports.run(ctx); rewinds++;
  }
  assert.equal(result, 1); assert.equal(yields, 2); assert.equal(completed, 1);
  assert.equal(calls - before, 3); assert.equal(shim.exports.asyncify_get_state(), 0);
  assert.equal(v.getBigUint64(ctx + 40, true), liveValue);
  assert.equal(generated.exports.r4.value, c.live ? poison : 0x44446666n);
  assert.equal(v.getUint32(ctx + 24, true), c.ic ? 77 : 0);
  compare(base, base + span, `Asyncify ${c.op} live=${c.live} ic=${c.ic}`);
}
console.log(`PASS: ${probes} C-oracle comparisons (${fast} SIMD, ${fallback} fallback), ${rewinds} real Asyncify rewinds/re-yields`);
