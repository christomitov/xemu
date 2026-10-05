# Mixed direct/legacy regions — authoritative status

## Delivered first gate

**`02d81397511b2ecc0ee5e5d087fb5ceb8dd33aab`**, branch `astra-direct-mixed-regions`.

Actual committed 30s mode2 headless passed final_JSON/rc0 (31.12s elapsed):
- 18,156,445 mixed-member checks, including **10,790,741 mixed memory TB checks**.
- Overall 68,446,763 memory accesses observed = checked; 27,669,045 region checks; **0 mismatches**.
- 445 mixed modules with 1,038 direct / 806 original legacy members; all direct-region modules 540 / 1,288 direct members, memory members900.
- 339 intro frames. PIT999–1001/s, vblank59–60/s, fully active APU1497–1503/s in inspected t6–11 window. No clock changes.
- **Legacy mixed rewind entries0, memory unverified0**: basic run is NOT actual rewind/cancellation coverage.
- Artifact `/tmp/astra-ic-mem-direct-mixed-mode2-final.json`, reported build `perf-12-15-g02d8139751`.
- Hash sent immediately to coordinator via herdr and user-visible commentary after positive coverage. No FPS/throughput claim from intro/check counts.

## Supplemental active and stress gates NOW PASSED

After initial BUILD_BUSY/EMULATOR_NODE_BUSY refusals, all three followups ran on the exact committed02d build, final_JSON/rc0,25s each (26.05s elapsed), good PIT/vblank/fully-active APU:
- **Active sampled smoke:**366frames,592mixedregions/1450directmembers,231509canonicalmisses. Named direct scopes3698/22368samples=16.53% over intro+idle, including reload322/spill22; tail32. NOT gameplay forecast, NOT lean FPS pair.
- **Mode2 eviction/unwind stress:**MAX256/TEST_UNWIND1000,229frames,108976407mixedchecks including104393496mixedmemoryTBchecks,0mismatch;16288evictions/31rewinds, **8 scoped own mixed-legacy rewind entries**. Memory cancellation NOW exercised: **10 unverified invocations**,155409723observed vs155409688checked (35 observations in cancelled invocations, not mismatches). This supersedes the earlier zero-cancellation evidence gap for the observer cancellation path, but does NOT establish after-effect MMIO resumption or identify every cancelled invocation as mixed.
- **ACTIVE eviction/unwind stress:**sameMAX256/UNWIND1000,254frames,22224evictions/15rewinds, **12 scoped own mixed-legacy rewind entries**,222292canonicalmisses. Exercises actual active legacy rewind adapter, not just mode2 coexistence. No independent full active architectural oracle/after-effect MMIO claim.

Artifacts `/tmp/astra-ic-mem-direct-mixed-{active-final,mode2-stress,active-stress}.json`. Machine gate record **`/tmp/astra-direct-mixed-headless-gate.json`**. Supplemental outcomes sent to coordinator. Source remains clean02d.

Coordinator integrated/deployed **EXACT02d detached**, no later opcodes. Their same-intro smoke busy_ms/frame mixed20.36 vsdefault22.73 (~-10%), mode2 30.16, allrc0; intro metric is not a gameplay result. Queued3leanpairs eachGR/SC/R6 mixed-on vsplain default, samebuild. Requested at leastone gameplaymode2pass/title before promotion. Defaults remain off; await data, hold x87 helper emission.

## Baseline / integration

Parent68baabd07b is ASTRA cherry-pick of user700675 codec v3. Prerequisites already on coordinator branch:
-1909abfdea direct sampled attribution
-be55687a13 CALL/PUSH immediate/moffs and decline histogram (ASTRA9655dfd54a)
-0d4c694f16 immediate register shifts (ASTRA3c3092942a)
-73c1cc3ebf decline ModRM detail (ASTRA17039ba4bc)
-7006759129 codec cc_shl/cc_sar fields/version3 (ASTRA68baabd07b)

Coordinator should pick ONLY02d813 for mixed implementation. **User's later9b5993 PUSH[m],28131e3 RMW andb02808b FS/GS/codec4 are NOT in this emulator gate.** A combined integration needs its own real mode2 gate. Do not blindly remove their changes to resolve conflicts.

Known followup needed for two-access forms: xwr_prepare still advances one original metadata site per memory INSTRUCTION. PUSHM-last-memory-op can pass with second site validated during emit resolution, but a subsequent memory instruction compares against PUSHM's second site and declines. Need per-access walk for PUSHM/mem_rmw, checking BOTH sites' insn, store/width, PC and exact final consumption. User acknowledged caveat; their aggregate mode2 pass alone does not identify PUSHM/RMW coverage. No such forms admitted/decoded in02d baseline.

## Interface and semantics

Default-off **XEMU_WASM_DIRECT_MIXED=1**, requires existing DIRECT_X86=1 or2, DIRECT_FLAGS=1, DIRECT_REGIONS=1; MEMORY=1 for memory members. Unset/0 retains all-members-supported behavior. No group selection/stealing/singleton policy changes.

- Ordinary whole group collected and published exactly as before. Unsupported members remain original TCG A/B within the SAME module. A fully unsupported group stays entirely original.
- Full initialized architectural bank remains resident across direct->direct edges. On entry to a legacy member, publish all8 GPRs + lazyCC packet (including RUNTIME CC_OP) + PC. **Per-member dirty flags cannot replace this full publication**, because they forget earlier direct writes.
- Invalidate private bank before any legacy instructions/helpers/polls can execute. Original legacy prologue/TCG locals/snapshot/GETPC/IRQ/A/B behavior remains authoritative. Reload canonical bank upon next direct entry. Original legacy memory/MMIO effects occur once.
- One private i32 bank-live local in heterogeneous groups only; no TB-header/context/serialization ABI growth, no cross-module bank ABI. No active direct softmmu calls/suspension.
- Mode2 executes the original bodies once. Reanchor expected bank at first supported BEGIN after legacy (AFTER original IRQ poll), retain independent expected bank across consecutive supported members; never publish shadow data to env. Existing observations check direct memory members without second guest reads/stores.
- Every direct member retains original-equivalent IRQ poll, runtimeCC and independent Jcc-slot verification. Unsupported IRQ exits are original; skipped supported members remain unchecked.
- Whole-module admission/import/size/CFG/instantiation failure rebuilds the SAME selected group as original TCG.
- Active memory miss stays precise canonical instruction-boundary exit + original COUNT1/no-goto retry, noNOIRQ, no prefix replay.

### Rewind correctness detail

Mixed module reentry starts bank-invalid; original legacy A delegates to original B. BEFORE interpreting ctx.tb_ptr's member index, if IC enabled and ctx.rewind_func differs from this module's IC-self, forward via existing chain.call import and return, matching original prologue semantics. Otherwise a foreign interrupted header's member index could accidentally select a DIRECT member in this module and trap/reexecute. This was found in source review and covered by an explicit colliding-index foreign-owner fixture.

For own direct mode1 member, do_init=0 still traps (no direct suspension supported). For mode2 memory reentry, cancel lost observer invocation as UNVERIFIED before original rewind/delegation. Owner forwarding uses original existing import path, not a new nested-stack scheme. It runs only on root rewind, not every legacy boundary.

## Counters / scopes

New lean functional counters:
- n_direct_mixed_build
- n_direct_mixed_members (direct members in mixed groups)
- n_direct_mixed_legacy_members
- n_direct_mixed_checked
- n_direct_mixed_memory_tb_checked (subset specifically memory-bearing supported members in mixed groups)
- n_direct_mixed_legacy_rewind (own legacy rewind-adapter entry, NOT completed B/ISR work or after-effect proof)

n_direct_region_members / n_direct_build now count actual direct members, not all members of a mixed module. Other existing region counts still describe whole groups. Lean n_direct_exec remains0 unlessTB_STATS1. n_guest_insn still is NOT valid crossarm normalization because direct-internal static-insn increments are absent.

DIRECT_PROFILE adds **direct:reload** and **direct:spill**. Include these with prior6 scopes for mixed totals. Reload includes fresh direct activation and return from legacy; spill is canonical bridge publication. Full sampler denominator includes phase_other/overflow; omitted scopes unknown. No per-access timer/counter. No JIT_PROFILE (disables direct admission); heavy dispatch diagnostics need TB_STATS1 + JIT_ENTRY_PROF1 separately.

## Tests/build

- Initial144 actual-builder mask/mode/profile/hint cases passed.
- Final **192** cases additionally exercise IC0/IC2 policies, original-B delegation without canonical-bank clobber, and foreign rewind-owner forwarding with colliding direct member index. Full GPR/lazyCC/PC final oracle for fixture CFG, alias entries, per-member IRQ/skips, changed/stale/foreign targets, whole-group off/fallback, all supported/unsupported masks. The B sentinel/foreign callbacks test delegation ONLY, not a real Asyncify helper or after-effect resumption.
- Legacy-region, glue and blueprint regressions passed on shift/codec-v3 baseline.
- `/tmp/astra-direct-mixed-{region-final,legacy,glue,blueprint}.log`.
- Full committed build `/tmp/astra-direct-mixed-build.log` passed. First earlier build attempt was refused EMULATOR_NODE_BUSY (not partial success), then retried successfully.
- Checkpatch0errors/3warnings (long lines/new-file notice), `/tmp/astra-direct-mixed-checkpatch.log`.
- Coordinator reported accidentally deleting ASTRA's marker while fixtures ran, their bench waited on held node lock. Logs show192passes and subsequent fullbuild pass; no guard kill. Coordinator agreed to honor existing markers going forward. Do not remove foreign markers.

## User gameplay context / priority

Before mixed, Mac sampled active direct total SC2.9%, GR2.0%, R61.9%, legacy jit72/76/46%. This supports addressing whole-group admission coverage but is not a demonstrated FPS gain from mixed. Boundary costs may erase benefit; keep defaults off, run gameplay mode2 then matched lean mixed on/off on SAME BUILD.

User's new decline histograms: x87 d9 dominates SC, GR61% (+FS TLS30%), R658% (+MOVSS42%). Requested **hold direct x87 helper emission** pending mixed A/B; mixed already leaves those members as original TCG. Decline frequency is not time share, and direct x87 helper calls need separate precise exception/GETPC/unwind/FPU-state design.

## Next steps

1. Await exact02d Mac3-pair/game lean A/B and gameplaymode2qualification. Intro active/mode2/stress followups are complete (above); no additional performance optimization yet.
2. Coordinator owns deployment/page/tooling and later opcode commits. Supplemental outcomes were sent promptly; exact02d deployed baseline avoids later opcode/codec confounds.
3. If integrating later forms locally, take coupled codec/frontend updates and explicitly fix/test per-access admission (PUSHM/RMW). Re-gate combined source. No claims that02d tested those later forms.
4. Keep lean/direct scopes distinct and preserve negative FPS results. No guest clocks/IRQ batching, mandatory engine flags or active helper continuations.
