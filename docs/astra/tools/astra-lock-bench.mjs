// Repeatable headless benchmark (Node, null renderer: measures the CPU side,
// which is the bottleneck). Guest time follows the virtual clock, so a slower
// emulator shows up as fewer guest frames per real second.
//
// usage: node tools/bench.mjs <boot|r6> [secs] [KEY=VAL env...]
//   boot  dashboard boot animation (no disc)
//   r6    Rainbow Six 3 demo from /root/workspace (disc inserted, reset)
//   sc    Splinter Cell demo from the same disc: menus -> gameplay, logs
//         flips/methods per second (BENCH_SAVE_AT=<s> saves a state there)
// Prints a summary and appends one JSON line to bench/history.jsonl.
// Run one at a time: flock /root/src/xemu-wasm/.node-run.lock node ...
import fs from 'fs';
import path from 'path';
import { execSync } from 'child_process';

const ROOT = '/root/src/xemu-wasm';
const WWW = `${ROOT}/www`;
const ISO = '/root/workspace/Tom_Clancy_s_Rainbow_Six_3__USA___Demo_.iso';
const [scenario = 'boot', secsArg, ...envArgs] = process.argv.slice(2);
const secs = +(secsArg || (scenario === 'boot' ? 40 : 150));
const jsPath = process.env.BENCH_JS || `${ROOT}/xemu/build/qemu-system-i386.js`;
const quiet = /^$/;

// BENCH_STATE=<file>: start from a saved machine state (xemu_wasm_save_state)
// instead of booting; BENCH_SAVE_AT=<sec>: save one at that time
const stateFile = process.env.BENCH_STATE ? path.resolve(process.env.BENCH_STATE) : '';
if (stateFile) envArgs.push('XEMU_WASM_LOADVM=bench');
const factory = (await import(path.resolve(jsPath))).default;
const M = await factory({
  noInitialRun: true,
  canvas: { id: 'canvas', width: 640, height: 480, transferControlToOffscreen() { return new ArrayBuffer(8); }, addEventListener() {}, removeEventListener() {}, style: {}, getBoundingClientRect() { return { left: 0, top: 0, width: 640, height: 480 }; } },
  arguments: [],
  print: (t) => { if (/abort|RuntimeError/i.test(t)) console.log('OUT ' + t); },
  printErr: (t) => { if (process.env.BENCH_VERBOSE || /abort|RuntimeError|XEMU-ERROR/i.test(t)) console.log('ERR ' + t); },
  onAbort: (m) => { console.log('ABORT ' + m); process.exit(2); },
  preRun: [(Mod) => { for (const kv of envArgs) { const i = kv.indexOf('='); Mod.ENV[kv.slice(0, i)] = kv.slice(i + 1); } }],
});
M.FS.mkdirTree('/xemu');
for (const f of ['mcpx_1.0.bin', 'Complex_4627.bin', 'eeprom.bin']) {
  M.FS.writeFile('/xemu/' + f, fs.readFileSync(path.join(WWW, f)));
}
// the 1 GB HDD image is read in place from the host (NODEFS, read-only use)
// through a small throwaway qcow2 overlay that takes the writes, instead
// of being copied into memory (which, with its I/O, cost gigabytes of RAM)
M.FS.mkdirTree('/hostwww');
M.FS.mount(M.FS.filesystems.NODEFS, { root: WWW }, '/hostwww');
const ovl = `/tmp/astra-lock-overlay-${process.pid}.qcow2`;  // per run: concurrent A/B arms
execSync(`qemu-img create -q -u -f qcow2 -F qcow2 -b /hostwww/${process.env.BENCH_HDD || "xbox_hdd.qcow2"} ${ovl} 8G`);
M.FS.writeFile('/xemu/xbox_hdd.qcow2', stateFile ? fs.readFileSync(stateFile) : fs.readFileSync(ovl));
fs.unlinkSync(ovl);
// the disc is read from the host file (NODEFS), not copied into the
// emulator's memory: keeps the r6 run's RAM close to the boot run's
if (stateFile) {
  M.FS.mkdirTree('/hoststate');
  M.FS.mount(M.FS.filesystems.NODEFS, { root: path.dirname(stateFile) }, '/hoststate');
}
if (scenario === 'r6' || scenario === 'r6snap' || scenario === 'sc') {
  M.FS.mkdirTree('/host');
  M.FS.mount(M.FS.filesystems.NODEFS, { root: path.dirname(ISO) }, '/host');
  M.FS.symlink('/host/' + path.basename(ISO), '/xemu/iso.iso');
}

const cstr = (p) => { let s = ''; for (let i = p; M.HEAPU8[i]; i++) s += String.fromCharCode(M.HEAPU8[i]); return s; };
M.__emscripten_proxy_main(1, 0);
const fields = cstr(M._xemu_wasm_stats_fields()).split(',').filter(Boolean);
const snap = () => {
  const v = new BigUint64Array(M.HEAPU8.buffer, M._xemu_wasm_stats_ptr() >>> 0, fields.length);
  return Object.fromEntries(fields.map((f, i) => [f, Number(v[i])]));
};
const phases = () => {
  if (!M._xemu_wasm_phase_top) return {};
  const o = {};
  for (const line of cstr(M._xemu_wasm_phase_top()).split('\n')) {
    const [th, rest] = line.split('|'); if (!rest) continue;
    o[th] = Object.fromEntries(rest.split(';').filter(Boolean).map((kv) => { const e = kv.lastIndexOf('='); return [kv.slice(0, e), +kv.slice(e + 1)]; }));
  }
  return o;
};

// per-thread CPU time (clock ticks) from /proc: contention-proof metric
const threadTicks = () => {
  const o = {};
  for (const tid of fs.readdirSync('/proc/self/task')) {
    try {
      const st = fs.readFileSync(`/proc/self/task/${tid}/stat`, 'utf8');
      const f = st.slice(st.lastIndexOf(')') + 2).split(' ');
      o[tid] = +f[11] + +f[12];      /* utime + stime */
    } catch {}
  }
  return o;
};
let ticksAt20 = null, flipsAt20 = 0;
const t0 = Date.now();
// hot-TB sampler (BENCH_HOT=1): which TB the vCPU is in, every ~1 ms
const hot = new Map();
const hotPtr = process.env.BENCH_HOT && M._wasm32_cur_tb_ptr ? M._wasm32_cur_tb_ptr() >>> 0 : 0;
const hotFrom = +(process.env.BENCH_HOT_FROM || 8), hotTo = +(process.env.BENCH_HOT_TO || 22);
const eipPtr = process.env.BENCH_HOT && M._xemu_wasm_eip_ptr ? M._xemu_wasm_eip_ptr(0) >>> 0 : 0;
const eipHist = new Map();
const hotIv = hotPtr ? setInterval(() => {
  const t = (Date.now() - t0) / 1000;
  if (t < hotFrom || t > hotTo) return;
  if (eipPtr) { const e = new DataView(M.HEAPU8.buffer).getUint32(eipPtr, true); eipHist.set(e, (eipHist.get(e) || 0) + 1); }
  const h = new DataView(M.HEAPU8.buffer).getUint32(hotPtr, true);
  if (h) hot.set(h, (hot.get(h) || 0) + 1);
}, 1) : null;
let prev = snap(); phases();
const rows = []; const vcpu = {};
await new Promise((resolve) => {
  const iv = setInterval(() => {
    const t = Math.round((Date.now() - t0) / 1000);
    if ((scenario === 'r6' || scenario === 'sc') && t === 2) M._xemu_wasm_request_disc(2);
    if (scenario === 'sc') {
      // the demo menu: R6, Ghost Recon, Splinter Cell -> Down twice, then
      // START through the SC menus/intro (as the Mac 'sc' preset)
      const press = (b) => { M._xemu_wasm_set_pad(b, 0, 0, 0, 0, 0, 0);
                             setTimeout(() => M._xemu_wasm_set_pad(0, 0, 0, 0, 0, 0, 0), 150); };
      if (t === 31 || t === 33) press(1 << 7);
      if (t >= 36 && t <= +(process.env.BENCH_SC_START_UNTIL || 70) && t % 2 === 0) press(1 << 9);
    }
    if (process.env.BENCH_SAVE_AT && t === +process.env.BENCH_SAVE_AT && !globalThis.saveStarted) {
      globalThis.saveStarted = true;
      console.log('--- saving state at t=' + t);
      M._xemu_wasm_save_state();
      const poll = setInterval(() => {
        const st = M._xemu_wasm_save_state_status();
        if (st === 2 || st < 0) {
          clearInterval(poll);
          if (st === 2) {
            const out = process.env.BENCH_SAVE_TO || `${ROOT}/bench/${scenario}.state`;
            fs.writeFileSync(out, M.FS.readFile('/xemu/xbox_hdd.qcow2'));
            console.log('--- state saved: ' + out + ' ' + fs.statSync(out).size + ' bytes');
          } else console.log('--- state save FAILED');
        }
      }, 200);
    }
    if (t === 10 && envArgs.some((kv) => kv.startsWith('XEMU_WASM_HELPER_PROF=1'))) { console.log('--- hprof reset at t=10'); M._xemu_wasm_helper_prof_dump(); }
    // menus -> gameplay: START then A, every 4 s from t=25 to t=105
    if (scenario === 'r6' && t >= 25 && t <= 105 && t % 4 === 1) {
      const btn = (t % 8 === 1) ? (1 << 9) : 1;
      M._xemu_wasm_set_pad(btn, 0, 0, 0, 0, 0, 0);
      setTimeout(() => M._xemu_wasm_set_pad(0, 0, 0, 0, 0, 0, 0), 250);
    }
    const cur = snap(); const r = { t };
    for (const k of ['n_flip', 'n_pgraph_method', 'n_tb_exec', 'n_helper', 'n_jit_compile', 'n_tb_linked', 'n_jit_relink', 'n_tb_selfloop', 'n_tb_gotoptr', 'n_tb_linkmiss', 'n_cpu_exit', 'n_apu_frame', 'n_present', 'n_jc_lookup', 'n_jc_miss', 'n_tb_gotoptr', 'n_guest_insn', 'n_tb_selfloop', 'n_tb_region', 'n_region_compile', 'n_region_members', 'n_jit_rewind', 'n_test_unwind', 'n_mmio_read', 'n_tb_gen', 'n_jit_evict', 'n_jit_instances', 'n_tb_invalidate', 'n_tb_flush', 'n_tb_recycle', 'n_tci_exec', 'n_tci_dropped', 'n_slow_st', 'n_slow_st_miss', 'n_slow_st_notdirty', 'n_smc_bitmap_miss', 'n_slow_st_mmio', 'n_slow_st_watch', 'n_slow_ld', 'n_slow_ld_miss', 'n_slow_ld_mmio', 'n_slow_ld_watch', 'n_tlb_alias', 'n_sse_checked', 'n_sse_mismatch', 'n_direct_build', 'n_direct_decline', 'n_direct_checked', 'n_direct_mismatch', 'n_direct_region_build', 'n_direct_region_members', 'n_direct_region_checked', 'n_direct_memory_build', 'n_direct_memory_members', 'n_direct_memory_observed', 'n_direct_memory_checked', 'n_direct_memory_tb_checked', 'n_direct_memory_unverified', 'n_direct_memory_miss', 'n_direct_flags_build', 'n_direct_flags_checked', 'n_direct_jcc_build', 'n_direct_jcc_checked', 'n_jc_flush', 'n_tlb_reset_dirty', 'n_tlb_fill', 'n_vblank', 'n_pit_tick', 'n_rep_movs', 'b_rep_movs', 'n_rep_movs_bail', 'n_slow_st_notdirty', 'n_smc_bitmap_miss', 'n_slow_st_watch']) r[k] = cur[k] - prev[k];
    if (M._xemu_wasm_lock_census_top) r.locks = cstr(M._xemu_wasm_lock_census_top());
    prev = cur; rows.push(r);
    if (t === 20) { ticksAt20 = threadTicks(); flipsAt20 = cur.n_flip; }
    if (t >= secs && ticksAt20) {
      const now = threadTicks(); let best = 0;
      for (const [tid, v] of Object.entries(now)) {
        const delta = v - (ticksAt20[tid] || 0);
        if (delta > best) {
          best = delta;
          globalThis.cpuThread = {tid, ticks:delta, name:fs.readFileSync(`/proc/self/task/${tid}/comm`, 'utf8').trim()};
        }
      }
      globalThis.vcpuCpuS = best / 100;              /* busiest thread = vCPU */
      globalThis.steadyFlips = cur.n_flip - flipsAt20;
    }
    if (process.env.BENCH_TRACE) console.error(`t=${t} flip=${r.n_flip} meth=${r.n_pgraph_method} mmio=${r.n_mmio_read} tbgen=${r.n_tb_gen} jit=${r.n_jit_compile} apu=${r.n_apu_frame} slowld=${r.n_slow_ld}`);
    globalThis.pd = cur.n_draw;
    const ph = phases().vcpu || {}; r.ph = ph;
    for (const [k, v] of Object.entries(ph)) vcpu[k] = (vcpu[k] || 0) + v;
    if (t >= secs) { clearInterval(iv); resolve(); }
  }, 1000);
});

// summary: guest fps over the busy part, worst 5 s window, work rates
const busy = rows.filter((r) => r.t > 5 && r.n_flip > 0);
const avg = (a, k) => a.length ? a.reduce((s, r) => s + r[k], 0) / a.length : 0;
let worst5 = Infinity;
for (let i = 0; i + 5 <= busy.length; i++) worst5 = Math.min(worst5, avg(busy.slice(i, i + 5), 'n_flip'));
const vtot = Object.values(vcpu).reduce((a, b) => a + b, 0) || 1;
const topv = Object.fromEntries(Object.entries(vcpu).sort((a, b) => b[1] - a[1]).slice(0, 16).map(([k, v]) => [k, +(100 * v / vtot).toFixed(1)]));
const loadStart = fs.readFileSync('/proc/loadavg', 'utf8').split(' ')[0];
let commit = '?';
try { commit = execSync(`git -C ${ROOT}/xemu-astra describe --always --dirty`).toString().trim(); } catch {}
// boot: the 3D logo animation = the seconds with heavy GPU work
const anim = rows.filter((r) => r.n_pgraph_method > 50000);
const tDash = anim.length ? anim[anim.length - 1].t + 1 : null;
let tbinfo = null;
if (M._wasm32_tb_info && M._wasm32_cur_tb_ptr) {
  const h = new DataView(M.HEAPU8.buffer).getUint32(M._wasm32_cur_tb_ptr() >>> 0, true);
  const ip = M._wasm32_tb_info(h) >>> 0; const dv2 = new DataView(M.HEAPU8.buffer); tbinfo = [0, 1, 2, 3].map((k) => dv2.getUint32(ip + 4 * k, true));
}
// CPU cost of the animation independent of guest pacing: share of vCPU
// samples not spent in self loops ("spin", mostly guest wait loops), and
// the busy vCPU time per guest frame (lower = faster emulator)
let spinS = 0, allS = 0;
for (const r of anim) for (const [k, v] of Object.entries(r.ph || {})) { allS += v; if (k === 'spin') spinS += v; }
const busyFrac = allS ? 1 - spinS / allS : 1;
const animProf = {}; for (const r of anim) for (const [k, v] of Object.entries(r.ph || {})) animProf[k] = (animProf[k] || 0) + v;
const animTop = Object.fromEntries(Object.entries(animProf).sort((a, b) => b[1] - a[1]).slice(0, 25).map(([k, v]) => [k, +(100 * v / (allS || 1)).toFixed(1)]));
const animHelpers = anim.length ? Math.round(avg(anim, 'n_helper')) : 0;
const heap = {
  top_mb: M._xemu_wasm_heap_used_mb ? M._xemu_wasm_heap_used_mb() >>> 0 : null,
  live_mb: M._xemu_wasm_heap_live_mb ? M._xemu_wasm_heap_live_mb() >>> 0 : null,
};
const res = {
  heap,
  jc_lookup_per_s: anim.length ? Math.round(avg(anim, 'n_jc_lookup')) : 0, jc_miss_per_s: anim.length ? Math.round(avg(anim, 'n_jc_miss')) : 0, gotoptr_anim_per_s: anim.length ? Math.round(avg(anim, 'n_tb_gotoptr')) : 0, tb_anim_per_s: anim.length ? Math.round(avg(anim, 'n_tb_exec')) : 0,
  insn_per_dispatched_tb: anim.length ? +(avg(anim, 'n_guest_insn') / Math.max(1, avg(anim, 'n_tb_exec') - avg(anim, 'n_tb_selfloop'))).toFixed(2) : 0, selfloop_anim_per_s: anim.length ? Math.round(avg(anim, 'n_tb_selfloop')) : 0,
  region_tb_per_s: anim.length ? Math.round(avg(anim, 'n_tb_region')) : 0, regions_total: rows.reduce((a, r) => a + r.n_region_compile, 0), jit_rewinds: rows.reduce((a, r) => a + (r.n_jit_rewind || 0), 0), test_unwinds: rows.reduce((a, r) => a + (r.n_test_unwind || 0), 0), mmio_reads: rows.reduce((a, r) => a + (r.n_mmio_read || 0), 0), region_members_total: rows.reduce((a, r) => a + r.n_region_members, 0),
  direct_totals: Object.fromEntries(['n_direct_build', 'n_direct_decline', 'n_direct_checked', 'n_direct_mismatch', 'n_direct_flags_build', 'n_direct_flags_checked', 'n_direct_jcc_build', 'n_direct_jcc_checked', 'n_direct_region_build', 'n_direct_region_members', 'n_direct_region_checked', 'n_direct_memory_build', 'n_direct_memory_members', 'n_direct_memory_observed', 'n_direct_memory_checked', 'n_direct_memory_tb_checked', 'n_direct_memory_unverified', 'n_direct_memory_miss'].map((k) => [k, rows.reduce((x, r) => x + (r[k] || 0), 0)])),
  slow_mem_anim_per_s: Object.fromEntries(['n_slow_st', 'n_slow_st_miss', 'n_slow_st_notdirty', 'n_smc_bitmap_miss', 'n_slow_st_mmio', 'n_slow_st_watch', 'n_slow_ld', 'n_slow_ld_miss', 'n_slow_ld_mmio', 'n_slow_ld_watch', 'n_tlb_alias', 'n_sse_checked', 'n_sse_mismatch', 'n_jc_flush', 'n_tlb_reset_dirty', 'n_tlb_fill'].map((k) => { const a = rows.filter((r) => r.t >= 10 && r.t <= 20); return [k, Math.round(a.reduce((x, r) => x + (r[k] || 0), 0) / Math.max(1, a.length))]; })),
  per_s_gen: rows.filter((r) => r.t >= 6 && r.t <= 30).map((r) => `${r.t}:${r.n_tci_exec}t/${r.n_tci_dropped}d/${r.n_tb_gen}g/${r.n_tb_recycle}r/${r.n_tb_invalidate}i/${r.n_jit_compile}c/${r.n_flip}f`).join(' '),
  clock_per_s: rows.filter((r) => r.t >= 8).map((r) => `${r.t}:${r.n_vblank}v/${r.n_pit_tick}p`).join(' '),
  repmovs_per_s: rows.filter((r) => r.t >= 8).map((r) => `${r.t}:${r.n_rep_movs}c/${Math.round(r.b_rep_movs/1024)}k/${r.n_rep_movs_bail}b/${r.n_slow_st_notdirty}nd`).join(' '),
  anim_profile: animTop, anim_helpers_per_s: animHelpers,
  busy_pct: +(100 * busyFrac).toFixed(1),
  busy_ms_per_frame: anim.length ? +(1000 * busyFrac / avg(anim, 'n_flip')).toFixed(2) : null,
  anim_fps: +avg(anim, 'n_flip').toFixed(2),
  anim_worst_s: anim.length ? Math.min(...anim.map((r) => r.n_flip)) : 0,
  t_dashboard: tDash, anim_secs: anim.length, last_tb_info: tbinfo,
  date: new Date().toISOString(), commit, scenario, secs, loadavg: [loadStart, fs.readFileSync('/proc/loadavg', 'utf8').split(' ')[0]],
  fps_avg: +avg(busy, 'n_flip').toFixed(2),
  // gameplay A/B metric: guest flips/s after warm-up (t >= 20 s)
  steady_fps: +avg(rows.filter((r) => r.t >= 20), 'n_flip').toFixed(3),
  // guest frames per CPU-second of the busiest (vCPU) thread, t >= 20 s:
  // insensitive to other load on the host
  flips_per_cpu_s: globalThis.vcpuCpuS ? +(globalThis.steadyFlips / globalThis.vcpuCpuS).toFixed(3) : null,
  vcpu_cpu_s: globalThis.vcpuCpuS || null,
  fps_worst5s: worst5 === Infinity ? 0 : +worst5.toFixed(2),
  frames_total: rows.reduce((s, r) => s + r.n_flip, 0),
  methods_total: rows.reduce((s, r) => s + r.n_pgraph_method, 0),
  tb_per_s: Math.round(avg(busy, 'n_tb_exec')),
  linked_per_s: Math.round(avg(busy, 'n_tb_linked')), relinks_per_s: Math.round(avg(busy, 'n_jit_relink')), selfloop_per_s: Math.round(avg(busy, 'n_tb_selfloop')), gotoptr_per_s: Math.round(avg(busy, 'n_tb_gotoptr')), linkmiss_per_s: Math.round(avg(busy, 'n_tb_linkmiss')),
  helpers_per_s: Math.round(avg(busy, 'n_helper')),
  vcpu_profile: topv,
  fps_last40s: +avg(rows.slice(-40), 'n_flip').toFixed(2),
  methods_per_s_last40s: Math.round(avg(rows.slice(-40), 'n_pgraph_method')),
  methods_k_by_10s: Array.from({ length: Math.ceil(rows.length / 10) }, (_, i) => Math.round(avg(rows.slice(i * 10, i * 10 + 10), 'n_pgraph_method') / 1000)),
  per_s: rows.filter((r) => r.t >= 6 && r.t <= 70).map((r) => `${r.t}:${r.n_flip}f/${r.n_apu_frame}a/${r.n_present}v`).join(' '),
  fps_by_10s: Array.from({ length: Math.ceil(rows.length / 10) }, (_, i) => +avg(rows.slice(i * 10, i * 10 + 10), 'n_flip').toFixed(1)),
};
if (envArgs.some((kv) => kv.startsWith('XEMU_WASM_HELPER_PROF=1')) && M._xemu_wasm_helper_prof_dump) { M._xemu_wasm_helper_prof_dump(); }
if (hotIv) {
  clearInterval(hotIv);
  const dv = new DataView(M.HEAPU8.buffer); const tot = [...hot.values()].reduce((a, b) => a + b, 0);
  const blocks = [...hot.entries()].sort((a, b) => b[1] - a[1]).slice(0, 400).map(([h, n]) => {
    const wp = dv.getUint32(h + 4, true), ws = dv.getUint32(h + 8, true);
    const ic = dv.getUint32(h + 48, true);
    let pc = 0;
    if (M._wasm32_tb_info) { const ip = M._wasm32_tb_info(h) >>> 0; pc = new DataView(M.HEAPU8.buffer).getUint32(ip, true); }
    return { share: n / tot, insns: ic, pc: pc.toString(16), wasm_b64: ws && ws < (1 << 22) ? Buffer.from(M.HEAPU8.slice(wp, wp + ws)).toString('base64') : '' };
  });
  fs.writeFileSync(`${ROOT}/bench/hot-boot.json`, JSON.stringify({ samples: tot, blocks }));
  { // guest kernel (0x80000000+) vs the rest, and the top blocks by guest PC
    const k = blocks.filter((b) => parseInt(b.pc, 16) >= 0x80000000).reduce((a, b) => a + b.share, 0);
    res.hot_kernel_pct = +(100 * k).toFixed(1);
    res.hot_top = blocks.slice(0, 25).map((b) => `${b.pc}:${(100 * b.share).toFixed(1)}%/${b.insns}i`).join(' ');
  }
  const etot = [...eipHist.values()].reduce((a, b) => a + b, 0);
  fs.writeFileSync(`${ROOT}/bench/hot-eip.json`, JSON.stringify([...eipHist.entries()].sort((a, b) => b[1] - a[1]).slice(0, 60).map(([e, n]) => [e.toString(16), +(n / etot * 100).toFixed(2)])));
}
if (M._xemu_wasm_count_top) res.events_top = cstr(M._xemu_wasm_count_top()).slice(0, 1500);
{ const m = process.memoryUsage(); res.mem_mb = Object.fromEntries(Object.entries(m).map(([k, v]) => [k, Math.round(v / 1048576)])); }
res.lock_census = M._xemu_wasm_lock_census_top ? cstr(M._xemu_wasm_lock_census_top()) : null;
res.cpu_thread = globalThis.cpuThread;
res.bench_js = jsPath;
res.bench_state = stateFile;
res.phase_samples_total = Object.values(vcpu).reduce((a,b) => a+b,0);
console.log(JSON.stringify(res, null, 1));
fs.mkdirSync(`${ROOT}/bench`, { recursive: true });
// the emulator settings this run used (stress/diagnostic runs are not
// comparable with default-config ones; the plot keeps them apart)
res.xemu_env = Object.fromEntries([...Object.entries(process.env), ...envArgs.filter((kv) => kv.includes('=')).map((kv) => [kv.slice(0, kv.indexOf('=')), kv.slice(kv.indexOf('=') + 1)])].filter(([k]) => k.startsWith('XEMU_')));
fs.appendFileSync('/tmp/astra-lock-history.jsonl', JSON.stringify(res) + '\n');
if (process.env.ASTRA_ROWS) fs.writeFileSync(process.env.ASTRA_ROWS, JSON.stringify({res,rows}));
process.exit(0);
