# Proposal only: one signed-saturating i16 SIMD lowering

## Selected item

One bounded generated-TB fast path for the existing TCG helpers
`gvec_ssadd16` / `gvec_sssub16` (PADDSW / PSUBSW): use Wasm
`i16x8.add_sat_s` / `i16x8.sub_sat_s` instead of calling their scalar loops.
This is one arithmetic lowering with an add/sub opcode choice, not a general
SIMD backend, decoder expansion, or revival of the direct translator.

**No implementation yet; awaiting approval.**

## Evidence

The kernel-inclusive steady SC profile on 8b8b3ce0b6 contains:
- helper_gvec_ssadd16: 312 / 35,602 samples = 0.876% vCPU self.
- helper_gvec_sssub16: 231 / 35,602 samples = 0.649% vCPU self.
- Combined: **1.525%**. This excludes caller argument marshalling and snapshot
  overhead; those have not been separately measured.

I extracted the actual TurboFan ssadd16 body from the retained jitdump:
`/tmp/astra-native-perf-2/gvec-ssadd16-native.asm` (512-byte native body).
It has scalar sign-extending word loads, add, upper/lower comparisons and
conditional moves in a loop, not packed saturating arithmetic. Source agrees:
`accel/tcg/tcg-runtime-gvec.c:1087` and `:1155`.

The x86 frontend already selects these helpers through ordinary gvec lowering
(`target/i386/tcg/emit.c.inc`, `tcg/tcg-op-gvec.c`). No guest decoding or memory
access scheduling needs to change.

## Proposed boundaries

- Separate default-off flag, tentatively `XEMU_WASM_GVEC_SSAT16=1`.
- Exact helper names and expected ABI only.
- Admit only decoded `oprsz == maxsz`, size **8 or 16 bytes** initially.
- 16 bytes: two vector loads, exact signed-saturating add/sub, vector store.
- 8 bytes: load exactly 8 bytes from each input (load64_zero), then store
  exactly 8 bytes. No speculative 16-byte access to an MMX-sized operand.
- Both inputs loaded before output. Identical aliases or disjoint operands
  only; reject unsupported overlap to the original helper.
- All other sizes/descriptors—including high-tail clearing—retain the
  original helper path. No change to helper semantics or helper ABI.
- Eligible operations touch existing CPU-state operands; guest memory
  operands, faults and MMIO remain the earlier, unchanged TCG/softmmu work.
- Preserve the original resumable fallback and its argument slots/GETPC,
  fresh-zero selector on rewind, region ownership, profiling and IRQ polls.
  Do not edit `wasm_handle_unwinding()` or dispatch/TCI behavior.
- Implement against the coordinator-approved current base, with LIVE_SAVE
  explicitly matched between comparison arms. No stale-baseline comparison.

## Why this is a Mac candidate

It removes scalar lane work in favor of a standard SIMD arithmetic operation;
V8's x86 and Apple Silicon backends both support native signed saturation.
It does not depend on HPET, host-clock reads, renderer behavior or BQL waiting.
This is a **modest ~1%-class candidate**, not a large-gain promise. The SC
self-time budget is 1.53%; the remaining vector work is not free, and callsite
savings and other-title shares are unmeasured. Mac proof remains required.

## Qualification if approved

1. Differential emitted-Wasm tests against the actual original C helpers:
   saturation boundaries, large deterministic/random lane sets, add/sub,
   both widths, identical/disjoint aliases and rejected partial overlaps.
2. End-of-linear-memory and canary cases proving exact 8-byte accesses;
   descriptor/max-size rejection and untouched surrounding bytes.
3. Original fallback behavior, including its rewind path, with LIVE_SAVE
   off/on; unchanged instruction/IRQ polling and metadata behavior.
4. Inspect generated/native code to prove the scalar loop/helper actually
   disappears on admitted paths and to check guard/code-size overhead.
5. Guarded same-build lean performance comparison and exact clocks; stop if
   it is neutral/slower. Coordinator-owned Mac A/B only if local gates justify
   it. No ASTRA Mac jobs.

No source changes, builds or new emulator runs were made for this proposal.
