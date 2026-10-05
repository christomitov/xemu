# ASTRA legacy-backend lock work — current handoff

## LATEST — sat16 implemented, negative gate; stop (supersedes below)
- User authorized bounded PADDSW/PSUBSW implementation, then requested rebasing onto perf-14. Current clean ASTRA branch `astra-gvec-sat16` at `74fa6fde12`, direct parent `65b75d6384` (LIVE_SAVE default-on / perf-14).
- User reports LIVE_SAVE Mac wins: SC29.4/28.7 and29.1/28.7, GR26.4/25.8, R6flat; R6 boot+mission load identical on/off. Coordinator owns perf-14 deployment. This supersedes prior LIVE_SAVE-pending notes.
- Sat16 remains default-off via `XEMU_WASM_GVEC_SSAT16=1`, exact ssadd16/sssub16, equal8/16-byte sizes; conservative alignment admits identical/disjoint spans and preserves original helper otherwise. No decoder, clocks/BQL, dispatch/TCI or unwinding-handler changes.
- Native emitter + native-emitter ASan/UBSan runs each pass819648 original-C-oracle comparisons and96 real standalone Binaryen Asyncify rewind/re-yield transitions;48 policy/LIVE_SAVE/IC configurations, ABI/name/descriptor rejection, aliases/canaries/end-of-memory. Existing phase and NOTDIRTY476/342720 gates pass. Full build+production postopt pass.
- Two same-build simultaneous SC60 pairs on perf-14, reversed startup order, LIVE_SAVE=1 in both arms: off/on flips/CPU-s6.849/6.751 (-1.43%),6.821/6.682 (-2.04%); mean **-1.73%**. All4 finalJSONrc0, clocks valid, minimum MemAvailable7.87GiB; no resource failures.
- Diagnostic-only helper wrappers confirm this is not universal fallback: off observed >=44.37M add/31.16M subtract original-helper calls; on imported helpers but zero calls observed. Diagnostic throughput is not speed evidence. Precise regression cause unestablished; no new native profile after negative gate.
- **Do not promote/integrate; stop here.** No Mac jobs, deployment, graph/catalog changes, mul16/pmaddwd expansion or renewed clock/BQL campaign. Candidate/tests and negative evidence retained.
- Report `/tmp/astra-gvec-sat16-result.md`; results `/tmp/astra-sat16-perf14-pairs.json`; logs `/tmp/astra-gvec-perf14-{tests,build,postopt}.log`; admission `/tmp/astra-sat16-admission-sync.json`.

## Historical deadline/lock handoff
- Clean production branch astra-bql-debug-deadline @8b8b3ce0b6 on perf-13 f7207958d3.
- Coordinator fast-forwarded, built and deployed8b. Mac result is NEUTRAL: SC new/old28.7/28.8,28.1/28.2; GR26.1/26.5 (round2 old off-route); R629.4/29.4,29.3/29.1. Kept as dead-work cleanup, not tagged or placed on graph. No ASTRA Mac jobs.
- Only production change: omit duplicate timerlistgroup_deadline_ns used solely by compiled-out debug ring. Real poll deadline and all timer dispatch remain unchanged.
- XEMU_WASM_DEBUG_DEADLINE_RESTORE=1 restores the old extra query. TRIPWIRE builds or replay always retain it.
- Native + ASan/UBSan each passed6912 actual-main_loop_wait scheduling/policy/replay/tripwire cases. Full build + production postopt passed.
- Lean same-build SC60 ABBA: old flips_per_cpu_s7.973/8.231 vs new8.606/8.323; means8.102/8.4645 (+4.47%). Old steadyFPS7.878/8.122 vs new8.512/8.220. All4 finalJSONrc0 and valid PIT/vblank/APU. Positive local/not-worse evidence, NOT a precise causal gain; did not transfer to Mac. NULL-renderer main-loop work inflates headless BQL cost.
- /tmp/astra-debug-deadline-abba.json, /tmp/astra-lock-deadline-{a1,b1,b2,a2}*, /tmp/astra-deadline-clean-{tests,build,postopt}.log.
- Plain-default smoke attempt refused BUILD_BUSY during coordinator's build: not a gate.
- Diagnostic branch astra-vcpu-lock-census now55f2bba510, parent da00444096 (subphase profiler). Do NOT ship diagnostic commits with the clean one-fix candidate.
- Subphase flag XEMU_WASM_BQL_DETAIL=1: scopes exclusive main-loop BQL-held ns, excluding release/wait intervals; nested scopes not double-counted. BQL wait snapshots identify active phase/callback. Detail alone avoids retry-probe counting. Three bounded tables/explicit overflow; all0 in capture.
- /tmp/astra-lock-detail-1.json and -analysis.json:1954 wakes/s. Held-ms/s: GUI32.96 (59.95/s), PIT18.71 (1999.95/s), OHCI6.81 (999.975/s), actual deadline15.06 and duplicate debug deadline14.97. PIT rising ticks999.975/s. No-op BH965/s merged with empty qemu_graphic_console_init by Binaryen; do not mislabel it recurring console initialization.
- Detail profiler perturbs: total vCPU wait97.66ms/s vs coarse40.88; no-op callback ~1.9us per invocation. These are attribution-only, NOT a new savings ceiling.
- PIT/GUI follow-ups REPORT ONLY, no changes: /tmp/astra-pit-gui-reading.md and /tmp/astra-pit-transition-audit.json. Snapshot confirms PIT mode2/count1125; low/high callbacks separated~889ns, both required by current PIC edge/pending-state semantics. GUI0.55ms is NULL-renderer fallback (VGA + MEMFS export); WebGPU already bypasses both. No Mac extrapolation or unmeasured subpart claims.

## Historical report-only follow-ups (superseded above)
- Native attribution complete: /tmp/astra-native-attribution.md. Explicit vCPU-TID user+kernel perf capture,35,602 analyzed samples,0 lost. This host clocksource is HPET. Clock-native path12.258% (helper_rdtsc callers10.994%, BQL timestamp callers1.003%), read_hpet self7.980%; compile/instantiate/lazy runtime0.764%; exception/longjmp0.593%. Native bulk memory0.194%, futex/notify0.239%, GC stacks0.017%. Do not extrapolate this HPET/syscall cost to Mac; no host clocksource or guest-clock changes.
- Coordinator deployed d2e7fa7541, opt-in XEMU_WASM_LIVE_SAVE=1; Mac A/B queued by coordinator. ASTRA source remains clean8b (no checkout or integration).
- Adversarial source review: /tmp/astra-live-save-review.md. No concrete missing-register rewind bug found across generic helper, i128, SSE fast/fallback, scalar ld/st, shared regions or IC routing. Explicit addr/data extras cover pre-emission dead inputs; R15/env retained; owner/BLOCK_PTR unchanged.
- Reproduced test regression was fixed by coordinator in35c45950a1 (matches wasm_save_regs prefix),476 checks PASS. Original failure log /tmp/astra-live-save-fixture-check.log. No ASTRA project changes made. Coordinator will use Mac A/B screenshots and, if positive, boot+R6-load headless LIVE_SAVE qualification before defaulting it; ASTRA source audit itself is not actual Asyncify qualification.
- New assignment: propose ONE generated-code optimization before coding. Selected proposal only: /tmp/astra-gvec-sat16-proposal.md, exact 8/16-byte PADDSW/PSUBSW helper lowering to Wasm signed-saturating i16 SIMD, unsupported shapes retain original fallback. Existing profile helper self share1.525%; extracted TurboFan ssadd16 body confirms scalar loop. No implementation/build/new emulator run; awaiting approval. No clock/BQL work or translator expansion.

## Historical source (holder census stage)
- Worktree /root/src/xemu-wasm/xemu-astra
- Branch astra-vcpu-lock-census, clean HEAD 46766c8812.
- Base f7207958d3 (perf-13, UNWIND_MEM now default-on).
- d62e35edaa: default-off XEMU_WASM_LOCK_CENSUS caller counts.
- ec407e8ad8: experimental spin-read, REJECTED; archived branch astra-bql-spin-rejected.
- 56996cfcb4 reverts that experiment. Current code contains no spin-read optimization.
- 46766c8812: atomic observed-holder + waiter interval diagnostics.
- To transfer diagnostics onto perf-13, take d62e35edaa and 46766c8812; do not take the rejected spin/revert pair.
- Coordinator owns wasm_handle_unwinding, TCI and dispatch changes. No ASTRA Mac jobs queued.

## Translator parked
- astra-direct-memory-miss-classify at 3f7259894b, tracked decision note docs/devel/wasm-direct-status.rst.
- Classifier code 1384ab20c1; prior 8fdd036f58 failed generic-backend build (TARGET_PAGE_MASK). 1384 passes metadata page mask explicitly.
- Native 2621440 classifier/request probes, 3960 memory hit/miss/reference tag-mutation probes, mixed/legacy/glue fixtures and full build passed.
- SC classification finalJSONrc0: t20–60 8243 misses/s; 38.7% aligned exact NOTDIRTY, 25.0% alignment, 25.0% page mismatch, 11.4% other flags, unknown0.
- No translator fix implemented. Coordinator explicitly shelved further work.
- Two already-running comparisons completed before stop: private same-build default/mixed steady 7.268/5.732 FPS. Not further translator investment.
- Details /tmp/astra-direct-mixed-gameplay-decision.md; /tmp/astra-sc-gameplay-{classify-1,default-1,mixed-1}*.

## Lock diagnostic method
- XEMU_WASM_LOCK_CENSUS=1, independently default-off. Single vCPU writer for caller/wait tables, atomically readable snapshots, bounded tables and explicit overflow.
- Counts actual existing mutex/trylock calls; no added probes, allocations, lock acquisition or clocks.
- All BQL-owning threads publish caller file-pointer+line in one atomic 64-bit word (Wasm32). Pre-unlock clears it; ordinary and timed condition-wait reacquisition are covered. BQL wrapper replaces generic try site with actual request site.
- At first observed failure, vCPU snapshots that holder, then records the EXISTING wait interval after acquisition. No new performance.now call.
- Output `wait|waiter-site|observed-holder-site|calls|total-ns` is cumulative.
- Crucial limit: entire acquisition WAIT interval is attributed to the holder observed near initial failure. It is NOT the full critical-section time and does not prove the same holder occupied the mutex throughout; unknown/release/helper windows remain visible.
- Tests: actual wrapper probe-count parity, config/filtering, bounded table/counter handling, all-thread publications, atomic pairing under concurrent publication, waiter interval accounting; native + ASan/UBSan passed.

## First caller census (pre-perf13, diagnostics only)
- /tmp/astra-lock-census-1.json, 60s, finalJSONrc0.
- t20–60: 11,487 BQL first tries/s, 1,212 contended acquisitions/s; 1,986,861 retry-loop attempts/s (1,985,649 busy).
- IRQ handling cpu-exec.c840 accounted for 967 contended acquisitions/s (~80%).
- 8.63M TB visits/s: not a lock on every TB.

## Spin-read experiment (rejected)
- Real mutex acquisition always remained mandatory. Publication after lock/before unlock, timed wait mirror, periodic real probe every64 read checks to tolerate stale hints.
- Tests passed: 80k protected increments, 200 real condition handoffs, timeouts, stale true/false cases, normal+sanitized; full build passed.
- Simultaneous 60s pairs hit the 5GiB memory floor, at5s before postopt and55s after postopt. Both were killed with no finalJSON: NOT performance results.
- User authorized serial ABBA with flips_per_cpu_s (busiest-thread CPU denominator, not completely contention-insensitive).
- Pre-perf13 off 7.621/7.646 vs on7.870/7.745, ~+2.3% signal.
- REQUALIFICATION on perf13: OFF8.228/8.205 vs ON7.836/8.256 flips/CPU-s; means8.2165/8.046 (-2.1%). steadyFPS OFF8.122/8.098 vs ON7.707/8.146. No repeatable benefit; coordinator rejected/cataloged it.
- All eight ABBA runs finalJSONrc0; steady PIT~1000, vblank~59.95, APU~1500. Thread comm is `node`; benchmark identifies the busiest thread, not by explicit vCPU TID metadata.
- Artifacts /tmp/astra-lock-{abba-*,perf13-*}*, /tmp/astra-lock-pair-spin{,-opt}-1*, /tmp/astra-bql-spin-*.log.

## Latest observed-holder result
- /tmp/astra-lock-holder-1.json; analysis /tmp/astra-lock-holder-analysis-1.json.
- 46766c8812, production postopt SHA256 00276f60c65332bb0db0cdefba67bdd055b0ade94f06f7f24523a638e6725088.
- SC60s; LOCK_CENSUS1/TB_STATS0/PROFILE0, no direct stages; finalJSONrc0.
- Window t20–60; both overflow counters0, no duplicate printed waiter/holder keys.
- Total attributed vCPU wait 40.8775 ms/s:
  - util/main-loop.c413: 32.8495 ms/s, 1159.925 acquisitions/s, 80.36%.
  - cpu-common.c164: 7.9260 ms/s, 51.85/s, 19.39%.
  - unknown/release: 0.0874 ms/s.
  - generic cpus.c606 try-wrapper window: 0.0113 ms/s.
  - pgraph.c964: 0.0033 ms/s.
- IRQ waiter cpu-exec.c840 -> main-loop.c413: 25.3986 ms/s, 932.95/s, mean27.224us. All IRQ wait 26.5009 ms/s.
- cpu-common.c164 is CONDITION-WAIT REACQUISITION inside do_run_on_cpu, not the original requesting caller. Old simple last-holder labels missed that distinction.
- Main-loop acquisition covers GLib dispatch, post-poll notifiers/timers, and next iteration's pre-poll preparation; site alone does not isolate callback cost.
- Steady clocks PIT1000.05/vblank59.95/APU1500.2. Diagnostic steadyFPS8.024; not an optimization measurement.

## Next sensible attribution (not yet implemented)
- Main-loop hold subphases/callback identity while vCPU waits, plus synchronous run_on_cpu initiator/work identity for the reacquisition bucket.
- Do NOT unlock around device/timer work, batch clocks/IRQs or assume one observed holder owned an entire wait.
- Any future optimization requires same-build gameplay A/B and exact-clock/progression checks.

## Tools/resources
- /tmp/astra-lock-bench.mjs: private clone of current tools/bench.mjs, correct private source metadata, per-PID overlay, full rows/census, steady_fps + flips_per_cpu_s + busiest TID/name.
- /tmp/astra-lock-run.py: serial shared Node lock + resource guard, finalJSON required.
- /tmp/astra-lock-pair-run.py: two simultaneous arms within ONE shared Node lock; same guards, combined RSS<=7.5GiB, MemAvailable>=5GiB. User allowed ABBA if pairs cannot fit.
- /tmp/astra-lock-tests.sh; /tmp/astra-ic-build.sh; /tmp/astra-postopt.sh. Unique-owner build marker cleanup, nice15/-j2/BINARYEN_CORES2/MALLOC_ARENA_MAX2. Node nice10 and one-second checks.
- Private builds now apply production standalone wasm-opt -O2 -g after linking (33MiB ->23MiB), no deployment.
- Pruned377.3MiB obsolete ASTRA binaries/smaps/derived dumps; manifest /tmp/astra-prune-20261005.json. Source, patches, decision/gate reports retained.
