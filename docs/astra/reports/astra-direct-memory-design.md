# Direct memory FIRST candidate — latest authoritative task/state

## LATEST: mixed regions delivered and deployed02d8139751

User explicitly authorized mixed direct/legacy members, superseding the prior homogeneous-admission-only constraint while preserving the ordinary whole-group partition. Current clean HEAD **02d81397511b2ecc0ee5e5d087fb5ceb8dd33aab**, branch **astra-direct-mixed-regions**. Full authoritative state **`/tmp/astra-direct-mixed-design.md`**, machine record **`/tmp/astra-direct-mixed-headless-gate.json`**. 192fixtures/fullbuild/actual positive mixed mode2 gate passed; active and mode2 eviction/unwind stress passed with12/8scoped mixed-legacy rewind entries. **Memory cancellation now exercised:10unverifiedinvocations/35observations left unchecked**, not after-effectMMIO proof. Coordinator deployed exact02d and queued3leanpairs/gameSC/GR/R6. Awaitgameplayresults/mode2qualification; defaultoff, no x87helper expansion yet. LaterPUSHM/RMW/FS-GS commits are NOT in02dgate and need per-access admission/combinedqualification. Prior historical candidate/evidence below retained.

## Previous follow-up: attribution delivered1909abfdea

Current HEAD/branch is **1909abfdea55459311a25ae62a7e163005453f5f / astra-direct-memory-attribution**, clean. Memory candidate55af remains unchanged except opt-in sampled scopes. Full status/results/limits: **`/tmp/astra-direct-memory-attribution.md`**. Mac SC/R6 memory mode2 passed; GR was menu-only and needs gameplay rerun. First lean SC/GR active pair was ~3–5% slower; R6control/pair2pending. User-reported gameplay misses SC67/s,GR246/s,R699/s. No established cause/optimization yet. Actual same-intro diagnostics show no dispatch inflation in a short preflush window; TB64 flush timings differ and confound broader comparisons. NewDIRECT_PROFILE1 labels body/CC/entry/miss with existing sampler; fullbuild,fixtures,actualactive25s withpositivecoverage passed, hashhandedoff. n_guest_insn isn'tsafe crossarm normalization (active internal increment omission). Keep sampling separate from lean pairs, preserve negative results. Prior qualification/state below retained.

## DELIVERED: gateable memory candidate55af23ba2c

**HEAD `55af23ba2c53a4f9e949b254ec8220a0c713ba8e`, branchastra-direct-x86-memory, clean.** The integration steps below are now IMPLEMENTED, not remaining WIP. Full committed build `/tmp/astra-direct-memory-build-final.log` and actual30s committed mode2 `/tmp/astra-ic-mem-direct-memory-mode2-final.json` PASSED final_JSON/rc0: **36,301,699 observed+checked accesses,9,234,469 memory TB checks,48 memory regions/83 memory members,9,572,963 total region checks,0 mismatch,412introframes**. Clocks PIT999–1001,vblank59–60,fully activeAPU1496–1504. Dashboard idle counts included; no productive-share/FPS claim. Reportedbuildperf-12-9-g55af23ba2c.

**Hash was sent immediately to coordinator viaherdr and user-visible reply.** Integration order oncoordinatorwasm(f128alreadythere): own`ddfc8097b6`, own`6848b4d032`, then`55af23ba2c`. ASTRA picks were11e6f1a829/d66e13dbc1. Full flagsDIRECT_X862/FLAGS1/REGIONS1/**MEMORY1**,TB64,TB_STATS0.

Same-production-source precommit ACTIVE20s smoke passed:9,500,825 activeexec(TB_STATS1),8,637 canonical memory misses,391frames,good clocks anddashboard progression. `/tmp/astra-ic-mem-direct-memory-active-wip1.json`. Not a performancepair or independentfullactiveoracle.

Supplemental25s committed mode2 MAX256/TEST_UNWIND1000 **after handoff** passed:34,331,720 memorychecks,0mismatch,24,528evictions,23JITrewinds,51forcedunwinds,260frames,good clocks. **Memory_unverified0: no actual cancellation coverage!** Do NOT conflate counter co-occurrence with a memory-member suspension/cancellation or after-effect replay proof. `/tmp/astra-ic-mem-direct-memory-cancel-stress.json`.

Detailed machine record `/tmp/astra-direct-memory-headless-gate.json`. Awaiting Mac memory mode2 then same-build lean active A/B. No source feature expansion or polish before measurement. Remaining correctness evidence gap: cancellation/fault/unwind path within an admitted reference memory member has not actually fired in these runs (explicitly unverified if it does); no active memory continuation exists in this stage.

### Implemented integration specifics (supersedes planning/WIP below)
- User observer64bit/open-begin follow-up6848 was imported asd66e13dbc1, parentof55af. No interface change to emitter: `resume_boundary` isnoop; `slow` is NONRETURNING canonicalmiss exit; `shadow` consumeswitness. Old emitter comments describe fullslowpath but integration explicitly does not implement it.
- Newprivate `xemu-wasm-direct-memory.h` defines64-site capture +default-offMEMORYgetter. No duplicateemitAPIs remain (user'sactualXwdMemAccess/Hooks inwasm32-direct.c.inc are authoritative).
- Backend records originalinsn,OI,restorePC,data/addrregs and target-derived TLBdescriptor/entry offsets/hash/alignment/span. Naturalalignment enforced; BSWAP/wideaccess declined. No RA/helper/restoreCC field needed or retained in currentprivatecapture.
- No-code relocs MEM_BEGIN9/MEM_RESUME10/MEM_END11 around ORIGINALaccess. BEGIN saves originaladdress beforeload can overwriteitsaddrreg; RESUME refreshes onlyonmiss fromstill-originaladdrreg; END observesactualdataReg aftercompletion/unwindcheck. Bdoesnotinjectthesehooks.
- Separate bounded LE customsection `xemu.direct-memory`version1 serializes14words/site,64max. Coreblueprint capturewire/version remains2/XWD_VERSION3; internaldecodedstructgrowth isnotwireABI. Blueprintwrite/read/find gain explicitallow_memory variants; oldwrappers remainregisteronly; genericscanner validates framing/duplicate names.
- Regionadmission optsdecode_ex ononlyMEMORY1, checksoriginalsite count/order/kind/width/restorePC againstplan, and enforces MEM_BEGIN->SUSPEND->RESUME->END for eachsite. Otherresumablehelpers stilldeclinewholegroup. Originalcollector/ownership unchanged.
- Memorygroups allocate XD_NLOCALS26 pluspending/addressprivate locals; reg-onlygroups keepoldlocalcount/pendingindex. e.mem pointsatpermemberhooks; e.access starts0 per emittedmember.
- Memorymode2 BEGIN startslog/pending butdefersshadowstepuntilfirstpendingVERIFY. ExpectedbankPC isfixedmemberPC/nonPCREL orretainedruntimePC/PCREL. VERIFYrunsbankstepconsumingobservations, closeslog, thenexistingfullGPR/FLAGS/PC+slotcheck. EveryoriginalMEM_END recordsonce. Duplicate/IRQ-skippedexitsnotchecked.
- `wasm32-direct-memory-runtime.c.inc` definesper-single-vCPU XwmoLog/purewrappers andactualcanonicalmissrequest. misspreservesexistingcflags_next_tb orusespubliccurr_cflags, replacesCOUNTwith1 +NO_GOTO_TB/PTR, neveraddsNOIRQ. FrontendalreadyrejectsCOUNT1. Misshookmaterializes e.cc intoXD_CC_OP ifflags_dirty, spillsfullpreinsnbank,overridesEIP=XD_PC+insnoffset,clearstb_ptr,callsbookkeepingleaf,returns0. No directmemoryhelper.
- `cpu_exec_setjmp` calls cancellation underCONFIG_TCG_WASM_JIT. Memorymode2 xwr_init allowsdo_init0 andcallscall-freecancelleaf beforeoriginalAprologuetailcallsoriginalB. Lost-bankinvocationclosedUNVERIFIED; BcontinuesoriginalTCGonly. Activeunexpectedrewindstilltraps.
- Newfunctionalstats n_direct_memory_build/members/observed/checked/tb_checked/unverified/miss. `checked`countsaccesses; `tb_checked`countscompletedmemorymembers. Buildcountersafterrealinstantiation. Mode2miss0expected; activechecked0expected. Unverifiedprefixobservedcounts mayexceedchecked; not a mismatch byitself.
- Updatedactual-source codec/glue/register-region/legacy-regionfixturespass `/tmp/astra-direct-memory-regression-{blueprint,glue,region,legacy}.log`. Existingfixturemocksabortifmemoryhookunexpectedlycalled. No additional new whole-builder memory fixture was added beforethis firstgate; relyonuser's2880emitterprobes+actualheadlesswithscope caveats above.

---
The following preserves task/design history; where it says WIP, not delivered, or next steps, current implemented/delivered state above supersedes it.

User priority: START NOW, no waits/check-ins, send gateable hash immediately after actual positive-coverage mode2 headless. User/coordinator works in parallel and has already supplied observer + decoder/emitter. Do not redo their work.

## Region stage DONE / Mac gate
- ASTRA5aba538951f2265576eea23689601498f001e91a (parents5751 metadata /967 bank), integrated as6c7b3a4330/59fc4668de/fb3fe42765, deployed.
- ASTRA30s mode2:7regions/17members,26168 region checks,23319 FLAGS/Jcc checks,0mismatch,407introframes; final_JSON rc0, clocks valid. `/tmp/astra-ic-mem-direct-regions-mode2-1.json`.
- Additional ASTRA20s ACTIVE diagnostic smoke:7/17,26049 active executions(TB_STATS1),407frames,final_JSON rc0. `/tmp/astra-ic-mem-direct-regions-active-smoke1.json`. Initial attempt refused BUILD_BUSY, later successful after marker cleared.
- Coordinator30s mode2:17members/28152checks/0mismatch; default0.
- Mac mode2 SC/GR/R6 PASS: region_checked avg42126/54699/19442 per second(max323k/111k/675k),0mismatch,noabort. Builds0.2–0.3/s,members max23/10/12, small coverage.
- Lean active/default SC25.4/24.5,GR26.1/25.5,R628.8/29.4: user calls neutral, standalone -11..-17% regression gone. No claim that memory MUST produce a speed gain.

## Current branch / files
Worktree `/root/src/xemu-wasm/xemu-astra`, branch **astra-direct-x86-memory** (region branch remains at5aba).
HEAD **11e6f1a829**, cherry-pick of user `ddfc8097b696007e13dcb927cd84c9c0e0309e8b` from branchclaude-direct-mem/worktree`/root/src/xemu-wasm/xemu-mem`.
Parent **2a76ca686a**, cherry-pick of user observer`f128905723f17616e3c611cbf5c15b43d10266f6` fromwasm/fb3fe42765.
Uncommitted ASTRA WIP (verify git status):
- new `include/qemu/xemu-wasm-direct-memory.h`: private XwdMemorySite/Capture + default-off XEMU_WASM_DIRECT_MEMORY getter. Temporary proposed duplicate XwdMemAccess/Hooks definitions were REMOVED after importing user's actual API.
- new `tcg/wasm32-direct-memory-meta.c.inc`: records original instruction restorePC/CC, memory insn ordinal/MemOpIdx/RA/helper/data_reg/addr_reg, bounded128; currently no wire serialization or admission use.
- `tcg/wasm32/tcg-target.c.inc`: includes private header/meta code; new static wasm_direct_memory pointer reset at TB generation; tgen_qemu_ld/st record site after ORIGINAL TCI opcode before Wasm operation.
- `tcg/tcg.c`: calls wasm_direct_memory_insn after filling each original gen_insn_data row, underCONFIG_TCG_WASM_JIT.
- `tcg/wasm32-direct-glue.c.inc`: starts private metadata capture before independent decode (including memory TBs currently rejected by old wrapper).
- `tests/unit/test-wasm-direct-glue.py`: stub for the metadata-start call.
**Full private build of this inactive metadata WIP passed before emitter import:** `/tmp/astra-direct-memory-meta-build.log`. No emulator memory gate, no memory admission, no production observation hooks yet. NEVER treat positive register-region checks as memory coverage.

## OVERRIDING FIRST-CANDIDATE DESIGN (user explicitly replaced earlier slow continuation scope)
**NO softmmu helper call from active direct code.**
- Inline live TLB tag/full span/alignment/flags check. Supply natural alignment (a_mask>=s_mask), so unaligned/cross-page/NOTDIRTY/MMIO/watch/invalid/other flags miss. NO NOTDIRTY leaf for this candidate.
- Hit: exactly one i32/i16/i8 host access. Commit architectural results only after success.
- Miss: bank is pre-instruction. Spill ALL GPR/full lazy-CC packet, canonical EIP=faulting instruction start; clear ctx.tb_ptr and return plain canonical exit0.
- Crucial: request one ORIGINAL-TCG instruction, **not simple redispatch** (otherwise same direct TB can miss forever). Helper may be a nonsuspending canonical-exit bookkeeping leaf, NOT memory access. Preserve pending cflags or use curr_cflags(cpu), replace COUNT with1 and OR CF_NO_GOTO_TB|CF_NO_GOTO_PTR. NEVER set CF_NOIRQ. Existing frontend rejects CF_COUNT_MASK, ensuring that retry can't be direct. No direct prefix replay.
- `curr_cflags` is public in `accel/tcg/internal-common.h`, definedcpu-exec-common.c38: cpu->tcg_cflags plus singlestep/one_insn_per_tb/logging flags. Use it, don't blindly read tcg_cflags.
- cpu_exec_loop picks cpu->cflags_next_tb (or curr_cflags), resets it to-1, and performs normal breakpoint checks. Interrupts remain enabled; if one intervenes the next dispatched instruction may be ISR; do not suppress IRQs to force identity.
- Initial forms: MOV8/16/32 load/store/immediate, MOVZX/SX loads, ALU r,[m], CMP/TEST[m],r/imm, PUSH/POP r32. Defer memory RMW, strings, segments/address-size overrides, compound stack. Existing frontend rejects HF_ADDSEG and other unsafe modes already.
- Mode2 executes ORIGINAL TCG once, observes completed accesses, then runs shadow TB using ALL observations. Do NOT re-read guest memory or sample the post-reference TLB to infer what was a pre-access hit. Expected bank retained across completed members.
- Original reference can still fault/unwind. First candidate explicitly CANCELS verification for that invocation; ORIGINAL B handles rewind with no shadow instrumentation. Do not claim those members checked or memory continuation qualified. Active code never suspends.
- Need cancellation at CPU longjmp recovery and on do_init0 entry to reference B. Close the log (cancel+end), increment unverified/cancel counter, never silently discard an active invocation at the next begin. Original B remains original and uninstrumented. Fresh subsequent A reloads canonical bank.
- xwr_init must allow mode2 memory-region do_init0 to bypass bank init and let the original A prologue tailcall B, instead of current trap. Mode1 unexpected rewind remains a trap.

## Parallel user ownership / imported code
### Observer f128 ->2a76
FilesONLY `include/qemu/xemu-wasm-direct-mem-observe.h`, `tests/unit/test-wasm-direct-mem-observe.py`.
`XwmoLog` per vCPU; begin(inv strictly increasing), observe(inv,pc,addr,oi,LOAD/STORE,value) AFTER COMPLETED original access; cancel(inv); shadow_load returns normalized original value; shadow_store compares relevant bits; end reports sticky error/UNVERIFIED/EXTRA/OK. Bounded64 events/inv; one invocation per TB/member, NOT entire32-member group.
User reports1,369,280 checks native+ASan/UBSan,20k random rounds; MemOp encoding asserted against actual headers. Pure bookkeeping, no guest access/callback/clock/replay.
**Follow-up COMPLETE:** user6848b4d0325f9950706a735629ef6fb8736495f1 imported asd66e13dbc1. uint64 invocation IDs; activebeginrejectedwithoutchangingopenlog.1,369,302native+sanitizedchecks including>2^32 throughUINT64_MAX. Supersedesearlieruint32API.
64-event bound is sufficient: admission caps plan.accesses<=64. No explicit per-access id needed for initial straight-line one-access-per-instruction scope; FIFO+PC/kind sufficient. Fault/unwind CANCEL not a completed check.

### Decoder/emitter ddfc ->11e6
User owns `include/qemu/xemu-wasm-direct.h` OPCODE/Insn/Plan only; `tcg/wasm32-direct.c.inc`; new`tcg/wasm32-direct-mem.c.inc`; new`tests/unit/test-wasm-direct-mem.py`. ASTRA owns all glue/backend/metadata/codec/region/bank/cancel integration.
- XWD_PUSH/XWD_POP appended; XwdInsn adds mem/mem_store/mem_width/disp; XwdPlan.accesses. XWD_VERSION remains3, serialized capture format not changed by those internal decoded structs.
- `xwd_decode_ex(c,p,allow_memory)`; old xwd_decode remains register-only. ASTRA must opt metadata writer/reader/frontend glue into decode_ex only whenMEMORY enabled.
- `xwd_emit_ex(p,l,verify,scratch,cc_helper,hooks,code)`; old emit NULL hooks declines memory. Register-only emitted bytes preserved.
- XwdEmitter.mem points to XwdMemHooks; .access is whole-TB ordinal. Existing xwd_emit_insns COPIES emitter into local e then returns it; hook sees the live local e.
- XwdMemAccess fields: insn,index,store,width,oi, mask_ofs/table_ofs(env-relative signed),cmp_ofs/addend_ofs(entry-relative),index_shift(PAGE_BITS-ENTRY_BITS),fold_shift12/fold_mask0xff000,page_mask,a_mask,s_mask,opaque.
- XwdMemHooks has opaque + resolve(e,insn,access,store,width,OUT_access)->bool; resume_boundary(e,a); slow(e,a,addr_local,value_local); shadow(e,a,addr_local,value_local).
**Use existing API without waiting for another emitter patch:** resume_boundary=no-op; slow=nonreturning canonical MISS EXIT, not helper. Message sent to coordinator. User's old comments still mention helper continuations; update scope comments when integrating, not behavior.
- Locals XD_MADDR22, MVAL23, MENTRY24, MHIT25, **XD_NLOCALS26**. Move region-private pending/address scratch beyond26, and allocate the extra locals in real A body. Current region code stillusesXD_TAKEN+1=22 and must change.
- Values zero-extended inMVAL; consumer doesmask/sign-ext; stores lowwidthbits. ESP updated onlyafteraccess;POP commits ESP+4 thendest (ESP destinationbecomesvalue). No GPR/lazyCC changes before memory hook.
- **Miss CC nuance:** bank_step updates runtimeXD_CC_OP only at TB end. A prior in-TB arithmetic op updates e.cc and packet locals but notXD_CC_OP. In miss hook, ife.flags_dirty, setXD_CC_OP=e.cc before fullbankspill. Incomingopcode otherwise retained. CanonicalEIP=member-entry XD_PC+originalinstruction byteoffset; don't change normalhit path or double-add bank_stepdelta.
- User reports2,880new hit/miss/verify probes vsregister-form oracle+declines, mutation catches16bitwidth/hostaddr; existing19992/612direct,bank/blueprint/region/gluepassed. These are not production fallback/observation/emulator proof.

## Immediate engineering next steps
1. Extend private XwdMemorySite capture with actual TLB layout/masks; map by original instruction index and accessordinal. Only1/2/4B admitted, matched plan memorykind/width/count, naturalalignment enforced, noextraMemory operations. Capture helpers no longer needed by active path (oldRA/CC metadata may be removed/deferred).
2. Add no-code original MEM_BEGIN/MEM_RESUME/MEM_END relocs. BEGIN before actualTLB access, RESUME inside originalresumable block beforehelper, END only after completed fast/slow path and unwindreturn check. Existing legacy buildersignore them; no originalexecutionchange when substitutiondeclines.
3. Observation alias issue: load output TCGreg may equal addressreg. Save ORIGINAL addr atBEGIN in a separate mode2 i32local. On slow-path RESUME (L32_1==0), re-read original addressreg BEFOREhelper; B isn't instrumented (cancelled) so no bank/address snapshot required. On fast path savedaddr survives. END usesactualresultdataReg/load orstoreoperand andsavedaddr.
4. Serialize bounded value-only memory metadata as separate customsection or extend versionedblueprint; do NOT growTBHeader/WasmContext. Current baseblueprintversion2 certifiesSUSPENDmarkers. Memory admission must match every suspend marker to a supported original memory site; reject other resumablehelpers and unsupporteddiagnostics asbefore.
5. Runtime pure wrappers likely inwasm32.c (single-vCPU enforced), prototypes inwasm32.h accessiblebackendforhelperfnindices: begin/observe/load/store/end/cancel plusmissrequest. UserAPIinv64Cwrappercanholdcurrentinv/log pervCPU; no generated clocks/allocations. Functional memory build/checked/observed/miss/unverified stats required; default-offMEMORY separatefromREGIONS.
6. Mode2 BEGIN now starts log/pending and defers shadow_step for memoryTB untilVERIFY. Run expectedstep onlyonce atfirstpendingVERIFY,consume/endlog,thennormalfullGPR/CC/PC+slotverifier. IRQ-skipped/duplicateexitnotchecked. OriginalBnotinstrumented, cancelledearly.
7. Active hooks use directfastemitter withresolved originalOI/TLB offsets; miss callback publishespreinsnstate, requestsreferenceCOUNT1 andreturns0. Existing originalgoto/IRQ/memberownershiprouting stays unchanged. Whole-group fallback prior execution on any metadata/codec/import/CFG failure; memory andreg-onlymembers share samearchitecturalbank, no mixedTCGlayout fusion.
8. Actual-source emittedfixtures MUST cover prefix-preserving miss, loadaddr/outputalias, push/popESP, can_do_io, flags beforemiss, forwardIRQ/memberroutes, no repeatedstore/MMIO, cancellation/unverified accounting and forced originalCOUNT1 (no redispatch loop). Then fullguardedbuild, actual mode2intro withpositiveMEMORYchecks/0mismatch; active smoke must exercise realmissprogress. Send hash immediately onceactualmode2passes. MacuserownsSC/GR/R6 gate+A/B.

## Source contracts inspected
- Original wasm_qemu_ld/st at tcg-target.c.inc~2230: TLB fast path, new resumable block, ifL32_1==0 pushenv/addr64/val/oi/originalTCIRA, saveTCGregs, callhelper, checkunwind, thenjoin. Fastload canoverwrite addrreg.
- Original TLB lookuphash `(addr ^ ((addr>>12)&0xff000)) >> (PAGE_BITS-ENTRY_BITS)` &live mask +livetable. Tag expected end-span/alignment. Pointer hints optin; initialdirectcanuseordinaryfreshlookup.
- `tcg/tcg.c` INDEX_op_insn_start fills gen_insn_data at~7140; ASTRA capturehookplacedAFTERallwordsfilled.
- `target/i386/tcg/tcg-cpu.c:x86_restore_state_to_opc` replacesEIP andSTATIC cc_op. Earlier fullslow design neededcompatibilityproof; **parked for currenthit/exitcandidate**.
- `cpu_exec_setjmp` accel/tcg/cpu-exec.c~1064 longjmpcatchincrementsn_cpu_exit thenphase/count andcleanup. Addcancel onlyunderWASM config; allfault/cancelinvocationsunverified.
- RegioncompileA/B loops inwasm32.c~1100/~1200: Bhasownreloc_b_ptr/count andhelper/ICremapping, currentlyignoresBEGIN/VERIFY. KeepBunverifiedoriginalwhenmemoryreferenceunwinds.
- Regionbuilderguard/RAMcounters/defaultIC2/wholegroupcollector unchanged. No prefix replay, no newyields, no perstoreclocks, no speculativeguestreads, no memoryrewindqualification claim.

Build/test/resource/identity rules remain exactly in `/tmp/astra-direct-x86-design.md` and conversation summary: ownmarker/serialNode/5GiBfloor/RSS7.5GiB/load6,TB64introonly,noR6local,noeditingcoordinatorworktree. CommitChristoMitovwithhooksdisabled,noautorpush/deploy. Coordinatorherdrw3:p1. Continue without waiting.
