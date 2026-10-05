# ASTRA sat16: do not promote (perf-14 headless regression)

## Candidate and baseline

- Candidate: `74fa6fde12`, branch `astra-gvec-sat16`, clean worktree in
  `/root/src/xemu-wasm/xemu-astra`.
- Direct parent: `65b75d638484566f14ded4ad67c5ec8644de27e0`, the coordinator's
  perf-14 baseline with LIVE_SAVE default-on. The benchmark's git-describe
  string `perf-12-23-g74fa6fde12` does not indicate an older baseline.
- Default-off `XEMU_WASM_GVEC_SSAT16=1` (first character `1` enables).
- Only exact `gvec_ssadd16` / `gvec_sssub16`, checked void(ptr,ptr,ptr,i32) ABI.
- Only oprsz=maxsz=8 or 16, canonical and max-size descriptor encodings.
  Operation-data bits are ignored, as in the original helpers.
- Conservative natural-size pointer alignment makes equal-size operands
  identical or disjoint. Unaligned/partial-overlap and all other descriptors
  retain the original helper, including its high-tail clearing.
- Both inputs loaded before output. 8-byte path uses load64_zero and
  store64_lane; it neither loads nor stores 16 bytes for MMX.
- No decoder, guest-memory access, clock, BQL, IRQ, dispatch, TCI or
  `wasm_handle_unwinding()` change. No mul16/pmaddwd expansion.
- The fast attempt precedes the existing resumable block; the ordinary
  helper path is selected on rewind by zero-initialized completion scratch.
  Existing register snapshots/owner bookkeeping remain. This does not
  eliminate the checkpoint itself.

## Correctness/build gates on perf-14

`/tmp/astra-gvec-perf14-tests.log`:

- Actual production emitter plus generic helper wrapper extracted into a
  native C fixture. Original C helpers compiled to Wasm as the oracle.
- **819,648 comparisons**: 787,328 SIMD / 32,320 fallback. Widths, signed
  boundaries, every signed-16 input against six representative second
  operands, lane patterns, exact aliases, partial overlaps, rejected sizes,
  data bits, maximum descriptor size, canaries and end-of-memory accesses.
- **48 configurations**: sat16 unset/empty/0/invalid/1/1yes, LIVE_SAVE off/on,
  IC off/on, both operations. Wrong names and ABI rejected before emission.
- **96 real Binaryen Asyncify unwind/re-yield transitions** through an
  instrumented parent -> manually resumable generated TB -> instrumented
  helper shim. Original C helper effects exactly once; live value and owner
  stamp retained. Descriptor deliberately made fast-eligible during rewind
  to prove the fresh fast attempt is skipped.
- Suite repeated with native emitter ASan/UBSan. The Wasm C-helper oracle is
  not itself sanitizer-instrumented. This standalone fixture does not claim
  full shared-region/foreign-instance emulator rewind coverage.
- Existing phase tests pass; NOTDIRTY 476 gates + 342,720 emitted-Wasm probes
  pass.
- Full private build and production-equivalent standalone wasm-opt pass.
  Logs `/tmp/astra-gvec-perf14-{build,postopt}.log`.
- Optimized Wasm SHA256:
  `340e20ade620deb034db440ed363ba676da59de3b45a1b958c4201c9456e522a`.
- Early fixture attempts caught a nonexistent i64-equality opcode macro in
  the initial candidate and a missing oracle fixture macro; both corrected
  before the successful final gates/build. Resource-refused final-test
  attempts are not counted as passes.

## Lean same-build SC gameplay A/B

Two simultaneous 60-second pairs, second with startup order reversed.
Same optimized build/state/NULL renderer; TB_STATS=0, PROFILE=0,
**LIVE_SAVE=1 explicitly in both arms**. Only GVEC_SSAT16 differs (0/1).
Standard busiest-thread CPU normalization; thread name alone is not an
independent vCPU identity check. No diagnostic JS wrappers in these runs.

| Pair | off flips/CPU-s | on flips/CPU-s | change | off/on steady FPS |
|---|---:|---:|---:|---:|
| 1 | 6.849 | 6.751 | -1.43% | 6.732 / 6.610 |
| 2 | 6.821 | 6.682 | -2.04% | 6.659 / 6.561 |
| Mean | 6.835 | 6.7165 | **-1.73%** | |

All four have final JSON and rc=0. CPU seconds: 39.42/39.40 and
39.29/39.36. PIT ~1000/s, vblank ~59.93–59.95/s, active APU
~1498.93–1499.73/s; no >1% guest-clock deviation. Minimum available memory
7.870/7.886 GiB; peak combined RSS 4.268/4.268 GiB. No resource failures.

Artifacts:
- `/tmp/astra-sat16-perf14-pairs.json`
- `/tmp/astra-lock-pair-sat16-perf14-{1,2}-{0,1}.json`
- Matching `-outcome.json`, `-rss.jsonl`, and per-arm logs.

## Small admission check (not speed evidence)

A private copy of generated JS wraps only the two original helper imports;
main Wasm is unchanged. Names/element table of the exact optimized Wasm map
ssadd16 to table slot2302/function4100 and sssub16 to slot2306/function4104.
No project source was changed for this diagnostic.

Separate 30-second runs, both final JSON rc=0:
- Off: at least **44,367,872 add / 31,160,207 subtract** wrapped calls by the
  last diagnostic snapshot. Sample arguments have desc512 (8-byte equal
  sizes), natural alignment and exact destination/source aliases.
- On: helper imports were installed (19 add / 11 subtract registrations),
  but **zero wrapped calls observed**, including immediate first-call
  logging. Thus the negative result is not explained by universal fallback.
- The raw SIMD-byte-pattern counter is only a heuristic (including false
  positives off); it is NOT a validated module/admission count.
- Initial console logging buffered in the busy worker, so its counts were
  incomplete. Repeated with synchronous stderr writes; only those count
  snapshots are used above. Diagnostic throughput is deliberately not used.

Artifacts: `/tmp/astra-sat16-diag/`, `/tmp/astra-sat16-admission-sync.json`,
`/tmp/astra-lock-sat16-admit-{off,on}-sync*`.

## Decision

**Stop and leave opt-in candidate unintegrated.** Both lean pairs regress.
No Mac job, deployment, tag, build catalog/graph edit, or scope extension.
SIMD/v128 operations and values in TB functions may inhibit TurboFan
optimizations or add spills (no new explicit v128 locals were allocated);
this is a hypothesis, not a measured cause. Guard/code-size/checkpoint
costs are also not free. No post-change native profile was pursued
once the negative gate was clear. Retain the commit, tests and negative
results rather than claiming a portable win from eliminating helper calls.
