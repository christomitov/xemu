# Latest task: memory-stage regression attribution

## User evidence / requested scope
- Deployed memory94eb8a13e1 (ASTRA55af + userddfc/6848).
- Mac mode2 SC/R6 passed: memory_checked avg2.47M/2.73M/s, maxmembers185,0mismatch/abort. GR273kchecks/s but stayedmenu; NOT gameplaygate, rerunneeded.
- Lean active pair1 SC25.3vs26.5off (-1.2);GR24.7vs25.4(-.7);R6on29.1/offpending. Pair2running; no cause established.
- User-reported last60s lean misses/s:SC67/GR246/R699. User suspectsdirectcode/routing ratherthanmissfrequency. Lowfrequency alone doesn't measure fullmiss-associatedcost, but no evidence of a dispatch-frequency storm.
- Asked cheap missrate+directtime attribution and actual sameintro on/off routing comparison. Do not guess FPS fromeligible/staticcounts.

## Delivered instrumentation
**HEAD `1909abfdea55459311a25ae62a7e163005453f5f`**, branch`astra-direct-memory-attribution`, parent55af23ba2c. Source clean. Hash/flags/results sent tocoordinatorviaherdr. No emulator feature/routing/memory changes.
- New default-off **XEMU_WASM_DIRECT_PROFILE=1** enables existing sampler evenPROFILE0/TB_STATS0.
- Active-only labels: **direct:entry**, **direct:mem**, **direct:reg**, **direct:mem-cc**, **direct:reg-cc**, **direct:miss**.
- Per-member existing generated/body/CCphase stores get newidentities afterlayoutcompatibilitycheck. Entryscope marksbankinit; missscope marksbankpublication/request. No peraccessclock/counter/import, no guestreads, no yields. Entryhasoneextraphasestore; missoneextrastore onlyonmiss.
- **direct:miss ends when returningtodispatcher. It excludes ORIGINALTCGretry, lookup/compile/other downstreamwork. Not totalmisscost.**
- Memberlabels classifywholebody (+IRQ/routing/spill) bywhetherthatmemberhasmemoryops, NOT isolatedTLB-operation time. PureCCcalltimeissplit.
- Mode2 referencebodiesretainoldlabels (not countedactive). Sum exactlythe6abovescopes; don'tsumall`direct:*` indiscriminately becauseexistingmode2`direct:verify` isn'tactiveexecution.
- ExistingUIphaseexports alreadycarrynames. NoUI/tooling fileschanged. FullvCPUdenominator mustincludetopN tail`phase_other`/`phase_overflow`; omittedscopesareunknown/notzero. Known sum/Nislowerbound; conservativemissingtailcanboundhigher. Samplershareincludeswaits/background phases, notnativecycles or removablecost.
- Cheap run: existingactiveDIRECT_X861/FLAGS1/REGIONS1/MEMORY1 +DIRECT_PROFILE1, **TB_STATS0**; functionaln_direct_memory_missalreadyworkslean. NoENTRY_PROFneededfortimeshare.
- Heavy routingdiagnostic: **TB_STATS1 + JIT_ENTRY_PROF1** (andPROFILE1 orDIRECT_PROFILE1). **NOT JIT_PROFILE**: thatknobdeliberatelydeclinesdirectregions.
- Keepboth kinds ofdiagnosticseparatefromsame-buildleanFPS pairs; samplingknown~1FPSoverhead.

### Qualification
- FullcommittedWasm build `/tmp/astra-direct-profile-build.log` passed.
- Policytest17cases/119emittedmodules inclDIRECT_PROFILE parsing, forceprofilewithPROFILE0/TB_STATS0.
- Actual-source registerregion24mode/scenario/hint/profilecases: helperphasematches, generatedscope restored, mode2notmislabelled, allrouting/IRQchecksretained. Legacy8casespassed.
- Logs `/tmp/astra-direct-profile-{policy,region,legacy}.log`.
- Actual25s activeNode intro withDIRECT_PROFILE1,TB_STATS0,PROFILE0 passed final_JSONrc0,402frames,9061canonicalmisses,positive44memoryregions/75members,validPIT/vblank/APU. `/tmp/astra-ic-mem-direct-profile-smoke.json` reportsperf-12-10-g1909abfdea.
- Samples whole117named-direct/22403=.522% plus20unknown-tail; intro(t6–10)22/4640=.474% plus15tail; idle(t17–24)2/7444=.0269%. Headlessintro/idle NOTMacgameplayforecast. No direct:misssamplesobserved; don'tclaimzerocost.

## Same-build intro comparison requested by user
Actual55af build,serialTB64smallHDD,30seach. Artifacts `/tmp/astra-ic-mem-memory-dispatch-{on1,off1,on2,off2}.json`;machinecomparison `/tmp/astra-direct-memory-dispatch-comparison.json`.
- Pair1PROFILE1+TB_STATS1 omittedENTRY_PROF1: n_jit_entries=0, unusablefordispatchcounts. ActiveexitcountersincrementunderTB_STATS butlegacyexitcountersneedENTRY_PROF; without itcomparisonsasymmetric. Coordinatornotifiedimmediately toaddENTRY_PROF1tobothqueuedMacdxmp arms.
- Pair2bothPROFILE1+TB_STATS1+ENTRY_PROF1,OFFthenON. FinalJSONrc0both,clocksvalid.
- **n_tb_exec countsALL entered TBvisits, includingregion/selfloop/TCIchains, NOTdispatches.** n_jit_entriescountsdispatcher-to-JIT calls, excludingICtailcalls. Reportboth.
- Commonpreflusht6–7ON/OFF: JITentries3.319M/3.510Mpersecond;TBvisits13.693M/13.898M;flips42.5/43;entries/flip78.09k/81.62k. No observeddispatchinflationinthisshortwindow; notproofaboutMacgameplay.
- Whole-runmeanbuiltmembers/region3.589/3.654; counts1005regions/3607membersON vs866/3164OFF. Notmaterialsmallerregionsestablished; differentwarmup/compilationhistories.
- **CriticalTB64confound:** pair2fullflushatt8ON vs11OFF. Pair1att11ON vs13OFF. Broadt6–11averagesmixdifferentflush/compilationstorms; NOT steady-workFPS evidence. On2excesscompilationtherecannotbeassignedtosteadyMacruntime.
- Idle t18–29entries/TBvisits .250052/.250065;IC2indirecthitsactive,notproductivework. No lostIC2pathdemonstrated.
- **n_guest_insn currentlyomitsactive-bank INTERNAL per-memberstatic-insnincrements**, whilelegacycounts them. Do NOT useitascrossarmworkdenominator. Notpatchedhere (diagnosticAPIcaveat, notguestclock/icount).

## Next
User/coordinator integrates1909smallattributionpatch andcollectsMaccheapdirectshares +samebuildheavyroutepair, awaitspair2leanandGRcorrectnessrerun. Useknown6scopes/fulltaildenominator. No optimizationcommitted basedonNodeintro alone. Sourceaudithypotheses only: activeinternaledgesusefullindexedownershipguard vslegacyfixed-targetguard; importedfullCCevaluationmaycostmorethanTCGpredicate-specialized lowering. Neither ismeasuredcauseyet. Nochanges toclocks/IRQ/memberpolls/wholegroupadmission/missretry/SMC/continuation.

Priorcomplete memory/regionstatus: `/tmp/astra-direct-memory-design.md`, `/tmp/astra-direct-memory-headless-gate.json`, `/tmp/astra-direct-x86-design.md`. Memoryreferencecancellationstillhaszeroactualcoverageinpriorstress (do notupgradeclaim); activehasnohelpercontinuation. Allpriorresource/identity/ownershiprulesremain.
