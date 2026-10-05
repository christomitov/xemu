# Adversarial review: d2e7fa7541 LIVE_SAVE

Reviewed exact commit d2e7fa75411a1fabb0592bbf44994be22bfca1c2, parent
8b8b3ce0b6. Report only: no project-source edits, checkout, build, emulator run,
or Mac job. Exact candidate backend extracted to
`/tmp/astra-live-save-tcg-target.c.inc` (SHA256
`ac441732cd5f367e899237b6521f0c188580706a56170b7b793253815ac46d37`).

## Verdict

**No concrete missing-register rewind bug found in the five existing resumable
emitter paths.** The distinction between TCG liveness and resumable-block
operand liveness is real; the explicit ld/st extras cover the discrepancy I
found. This is a source audit, not dynamic Asyncify qualification.

**One reproduced validation regression:**
`tests/unit/test-wasm-notdirty-emitter.py:30` searches the store body for
`wasm_save_regs(`. The candidate now contains `wasm_save_regs_live(`, so the
fixture raises `ValueError: substring not found` before running any tests.
The intended fresh-prefix / resumable-helper ordering assertion should be
retained with the new call spelling. No source fix was made here.
Reproduction: `/tmp/astra-live-save-fixture-check.log`; ran the unchanged test's
import-time logic against the exact candidate backend via a read-only text
substitution, with bytecode-cache writes disabled. No compiler/emulator ran.

## Why allocator liveness alone would not suffice

`tcg/tcg.c:5672-5677` marks dead input temps dead **before emission**.
Output registers are assigned at `:5754-5757`, the instruction is emitted, and
output death/synchronization happens only afterward (`:6108-6120`). Thus a
load/store's input can still be needed by its rewind entry despite having no
`reg_to_temp` mapping. Conversely, an output mapping can already exist before
its value has been produced; saving that old local is harmless only because
the operation redefines it after the resumed helper returns.

The candidate saves `reg_to_temp != NULL`, reserved regs, explicit env, and
path-specific extras. Reserved R15 is the TCG call stack; R13 is scratch; R14
is env. `tcg_reg_alloc_start` clears the map even for fixed temps, so the fixed/
reserved handling is necessary, not redundant reasoning from the map alone.

## All resumable paths

Line references below are in the candidate `tcg/wasm32/tcg-target.c.inc`.

| Path | Registers re-read on replay | Assessment |
|---|---|---|
| Generic helper (`:1849-1870`) | R15 for argument slots; ctx for GETPC | R15 is reserved and retained. Helper input registers can be dead because arguments were copied into the call stack before the allocator drops them. |
| i128 fast/fallback (`:1644-1664`) | R15 for arguments, by-value pointer, sret/result slots | Retained; no additional dead TCG operand read found. Fresh rewind scratch `L32_1=0` selects original fallback, not the fast attempt. |
| Scalar qemu_ld (`:2321-2336`) | env and original address | Explicit `BIT(addr)` is necessary and sufficient for the visible replay prefix. |
| Scalar qemu_st (`:2364-2383`) | env, address and store value | Explicit `BIT(addr) | BIT(data)` covers both dead inputs and same-register aliases. |
| SSE fast/fallback, the ~4410 path (`:4440-4499`) | env/R14 and stack/R15 | Both retained. `sse_arg` (`:3905`) loads from R15, not former argument registers. No additional TCG-register dependency found. |

### Aliasing and helper outputs

- For a slow load with `data == addr`, the snapshot occurs before the helper
  and before `wasm_set_r(data)`. It retains the original address even if an
  unwinding helper returns a placeholder that overwrites the local. Rewind
  restores the snapshot before rebuilding arguments; a successful return
  overwrites the output again. The fast load cannot have overwritten it on
  the slow branch (`L32_1 == 0`).
- A load whose output is distinct but dead is still correctly executed for
  side effects. Its unused previous destination value is irrelevant.
- Store address/data aliases need one saved register; disjoint dead operands
  need both bits. Both are covered, including the complete i64 store value.
- Helper arguments are marshalled before `temp_dead` and call-clobber handling
  (`tcg_reg_alloc_call`, `tcg.c:6293-6337`). The backend has no register argument
  ABI. R0/R1 are call-clobbered; live old values are spilled before the call,
  and output mappings are assigned only after `tcg_out_call` (`:6355-6369`).
  Consequently, an output aliasing a dead input does not require preserving
  that input register in addition to its already-marshalled argument slots.
- i128 helper return uses R0/R1 loaded from the stack (`wasm_call_results`);
  both values are defined on completion. i128 by-value inputs occupy argument
  slots, addressed via R15. There is no separate implemented qemu_ld2/st2
  emitter with another hidden second-half register dependency; scalar pieces
  emitted as separate TCG ops use ordinary inter-op liveness.

### Scratch locals and the SSE fallback

The mask covers TCG LREG locals only. L32/L64 scratch locals were not saved by
the old full-TCG-register snapshot either. On rewind the new Wasm invocation
zero-initializes them. i128 and SSE use this deliberately: L32_1=0 selects the
fallback. In SSE verify mode, the resume prefix reconstructs its saved flags
from env; zero L32_1 suppresses comparison against the now-lost fast-result
scratch. This is unchanged by LIVE_SAVE. It is not evidence that arbitrary
scratch state may safely cross a newly added suspension site.

## Regions, IC and snapshot pruning

- `wasm_save_regs_live:578-583` still saves BLOCK_PTR and, with IC enabled,
  writes the actual instance's `ic_self` into `ctx.rewind_func`, regardless
  of how many TCG registers were selected.
- The B prologue (`:4895-4944`) checks owner / forwards foreign rewind before
  restoring globals, then restores BLOCK_PTR. No eliminated TCG register is
  used for that owner decision: it uses ctx, headers and IC globals.
- Shared-region construction (`tcg/wasm32.c:1140-1210,1233-1273`) copies member
  entry and B bodies and remaps calls/IC sites. It does not derive a new
  register mask from the region builder's allocator state. The mask remains
  the one emitted at that member's original TCG program point.
- Region/member routing reconstructs its private cursor from ctx/header data;
  it does not need an unsaved TCG register to locate the interrupted member.
  Stale globals from another member are harmless only for the registers that
  are dead at the actual interrupted program point—the reviewed mask preserves
  that condition under existing single-vCPU instance ownership assumptions.
- Snapshot pruning (`:5205-5220`) still works on recorded snapshot pairs;
  positions are adjusted after dispatch insertion (`:5120` vicinity). The
  deleted pairs were never emitted, so no fixed-size/count assumption was
  found. Operand reads outside `in_snapshot` mark `used_regs`; ld/st extras
  cannot be discarded by SNAPNOP as "never used". Env and call stack are also
  explicitly retained by its keep mask.
- IC / link terminal forwarding does not resume an old caller's register
  continuation. The target that actually suspends owns the snapshot. The
  unchanged owner word and BLOCK_PTR remain essential.

## Remaining qualification gap

The new commit adds no focused register-poison / actual-rewind fixture. Gameplay
FPS and exact clocks do not establish that the rare dead-input cases rewound.
Before promotion, the targeted coverage I would want is:

1. Distinct dead address and store value; load destination aliasing its dead
   address; aliased store address/value; unrelated live registers surviving.
2. Real yield and re-yield inside the same slow helper, with saved globals
   pre-poisoned differently from current locals. Check original arguments,
   completed results and exactly-once device side effects.
3. Generic helper dead inputs plus i128 slow load/store (including a later
   subaccess yielding), checking R15, argument slots and both result halves.
4. Suspension in a non-first shared-region member and forwarding through a
   different instance that also contains the TB; verify owner and B selection.
5. SNAPNOP on/off, IC off/on and the fast/fallback branches. The existing
   NOTDIRTY admission fixture explicitly does not execute Asyncify rewind;
   repairing its spelling is necessary but is not that missing qualification.

No production fix is proposed from this audit, beyond reporting the fixture
regression for its owner. I have not modified the coordinator-owned unwind,
TCI, dispatcher, or live-save implementation, and have not reopened translator
work. Current ASTRA worktree remains clean at 8b8b3ce0b6.
