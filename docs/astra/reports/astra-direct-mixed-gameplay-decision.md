# Mixed regions: gameplay rejection and bounded follow-up

Baseline: exact 02d81397511b2ecc0ee5e5d087fb5ceb8dd33aab, all new direct stages opt-in.

## Mac lean decision

Coordinator-reported matched gameplay FPS:
- GR: mixed 22.3/23.0 vs default 25.4/26.6.
- SC: mixed 23.3/23.3 vs valid default 28.5. Second default went off-route; exclude it.
- R6: mixed 28.5 vs default 29.5.

Clear regression; do not promote. Third pairs cancelled. Intro 20.36 vs 22.73 ms/frame does not override gameplay.

## Miss rates: final reports, last 60 s

Units are already per second, not totals:
- GR 20261004-200100-hot.json: 207648/s; 202412: 209110/s.
- SC 200854: 67166/s; 203205: 67341/s.
- R6 201618: 58388/s.
- Mean clocks near PIT1000/vblank59.94/APU1500.

Historical intro 231509 was a TOTAL over 25 seconds (~9260/s), not 231k/s. Old homogeneous-memory SC67/GR246/R699 misses/s do not describe mixed regions.

## Separate DIRECT_PROFILE captures

SC 204311, last30s vCPU percentages:
- legacy jit54.5; direct mem14.1, reg3.4, mem-cc2.0, reg-cc1.4,
  spill0.6, reload0.4, entry0.2; dispatch10.6.
- Named direct sum22.1%. Last60s misses13689/s, substantially different from lean.

GR 204723:
- legacy jit60.1; direct mem12.1, reg2.7, mem-cc1.6, reg-cc1.1,
  spill1.0, reload0.4, entry0.3; dispatch8.9.
- Named direct sum19.2%. Last60s misses187464/s.

Do not renormalize listed percentages: report truncates to top20, phase_other is included in the full denominator, omitted labels are unknown. Named spill/reload ~1–1.4% does not support those copies alone causing the lean regression. External-exit spills and native register pressure are not separately isolated. direct:miss excludes subsequent COUNT1 lookup/original retry costs. These samples do not show that direct code is cheaper than the original code for identical work.

## Latest user authorization (supersedes immediate shelving)

Miss classification, then AT MOST ONE targeted fix. Judge on same-build/default comparisons using the new fast SC saved-state gameplay benchmark. If the fix does not beat default there, shelve translator; no dirty-mask, neighbor-admission or decoder experiments. Even a positive headless result is not Mac promotion evidence.

Benchmark supports BENCH_JS. User explicitly authorized SC gameplay restore via scenario r6snap (disc contains multiple demos):
BENCH_STATE=bench/sc-gameplay.state node --experimental-wasm-branch-hinting tools/bench.mjs r6snap 60 ...
State is currently 72MiB. No menu inputs in r6snap.

Private clones preserve benchmark window calculations but correct private-build metadata, output full rows/all n_direct_* counters and use private artifacts:
- /tmp/astra-sc-gameplay-bench.mjs
- /tmp/astra-sc-gameplay-run.py

Runner holds shared Node lock, checks foreign build marker, rejects load>=6/foreign emulator, samples resources every second; stops below5GiB available or above7.5GiB RSS. Final JSON required. With sampling off busy_ms_per_frame uses busyFrac=1 fallback and is not an independently measured busy-time statistic. fps_avg also excludes zero-flip rows; inspect full rows/zero intervals and matching windows.

## Current classifier WIP

Branch astra-direct-memory-miss-classify, parent02d. No optimization yet.
- Default-off XEMU_WASM_DIRECT_MISS_CLASSIFY=1, active mode1 only.
- Capture exact already-loaded TLB tag via one diagnostic-only local.tee before comparison; no extra TLB/guest read and no new hot per-access counter.
- On an actual miss, classify load/store and five exclusive reasons: alignment (priority), aligned exact NOTDIRTY store tag, page mismatch, other flags, other.
- Extra notdirty_tag counter overlaps reason classes and does NOT imply leaf eligibility/success.
- Original canonical publication/COUNT1 retry remains unchanged. Mode2 never captures tags or re-probes reference memory.
- New test-wasm-direct-miss.py checks actual classifier/request code and runs existing Wasm memory oracle while mutating live TLB tags after probe; recorded diagnostic must remain original, reference must not capture.
- No edits to tcg/wasm32.c or tcg/wasm32/tcg-target.c.inc. Coordinator owns wasm_handle_unwinding / UNWIND_MEM experiment.

Test attempts so far refused EMULATOR_NODE_BUSY (not passes). Latest lock was free but resource guard found coordinator Node PIDs2971484/2971529, each ~2GiB RSS, both r6snap70. Coordinator notified; no unrelated processes touched. /tmp/astra-miss-tests.sh cleans only its uniquely owned marker.

Specific candidate, only if classification supports it: original TCG uses audited helper_wasm_notdirty; direct memory currently exits canonically on all NOTDIRTY tags. Exact-tag counts alone do not prove the leaf will succeed or quantify retry costs. Reusing that leaf would require all original config/debug/plugin/parallel/SMC_DUMP/coherence/dirty-client safety guards, no suspension/fault/helper replay and exactly one store after success. No such fix is implemented yet.
