# Opt-in background JIT compilation — design for review

Status: design only; no implementation, build, runtime experiment, or FPS claim.
Reference: integrated `9619275e23`; relevant TCG/lifecycle files match ASTRA
`0ac0670795`. This follows NOTDIRTY_INLINE; it is not a direct-x86 translator,
JSPI conversion, Asyncify exclusion, or timer optimization.

## Recommendation

Separate **prepare → compile → instantiate → publish**. Start with
`WebAssembly.compile()` from the owning vCPU worker, bounded pending work,
and TCI continuation. A plain helper worker is an alternative, not a prerequisite:
V8 may already do the engine compilation off-thread for the Promise API.
Instantiate and publish only on the original vCPU worker at a validated fresh
entry boundary. Preserve default shared regions and IC=2.

**First feasibility gate: completion delivery.** A Promise does not make its
`.then()` callback run while its worker remains in a long-running Wasm/C call.
A helper worker can finish compiling, but its Module message still needs the
vCPU worker's event loop. Current `trysleep()` only calls `emscripten_sleep(0)`
at the instance limit. That cannot be assumed to bootstrap asynchronous
compilation when the first pending jobs prevent reaching that limit.

Do not implement “compile async, poll a flag” and assume it solves this.
Prove progress with existing task yields first. If that fails, obtain approval
for a narrowly bounded host-task service point at an audited CPU boundary;
do not silently introduce new sleeps, change guest polling, or await a compile.

## 1. What actually stalls today

In `tcg/wasm32.c`:

- `compile_tb()` calls `wasm32_instantiate()` synchronously.
- `compile_region()` performs selection/relocation/assembly synchronously, then
  calls the same function. It immediately frees its temporary buffers afterward.
- `wasm32_instantiate()` binds helper functions from the owner-local `wasmTable`,
  copies bytes out of shared memory, calls `new WebAssembly.Module(bytes)`,
  and calls `J.install()`.
- `J.install()` creates the Instance, IC table/globals, and function-table entry.
  C then assigns instance ownership and shared-region aliases.
- Current `ns_jit_compile` includes multiple stages, not just engine compilation;
  the region path also includes region assembly.
- Cold chaining in `wasm32_tci_chain()` returns to the dispatcher when a TB has
  an instance or reaches the compile threshold. Leaving a pending TB in that
  state unchanged would repeatedly dispatch instead of efficiently chaining TCI.

The reported 4,400 translated TBs and 1,395 JIT compiles in one intro second are
motivation, not a breakdown of CPU time. Frontend translation/register allocation,
TCI/Wasm byte generation, and initially region assembly remain synchronous.
Instantiation and first-use/lazy function compilation may still stall after a
Module Promise resolves. Background TurboFan tier-up may already be asynchronous.

Measure stage distributions and actual first-entry latency; do not promise that
all of `jit_compile`, all new-effect stalls, or all first-walk stalls disappear.

## 2. Execution and ownership rules

1. Default/unset `XEMU_WASM_JIT_ASYNC` retains the existing synchronous pipeline.
   `=1` enables the experiment. Set before startup; record in `xemu_env`.
2. Background work consumes immutable module bytes only. No guest execution,
   CPU state, instruction fetches, memory callbacks, table mutation, or Instance
   creation on a compiler worker.
3. While pending, execute the existing TCI path, or an already-owned compiled
   instance if there is one. Never restart a partially executed TB.
4. Existing compiled instances win. Never replace a live/warming instance merely
   because an asynchronous result arrives. Initially only compile unowned TBs.
5. Promise/message callbacks only record bounded completion metadata and retain
   a Module in a JS ready map. They never dereference a TB/header, call guest
   code, publish C ownership, evict an instance, or fill an IC.
6. Only the owning vCPU worker binds imports, instantiates, calls `addFunction`,
   and publishes ownership. Worker-local Emscripten tables, Asyncify state, TLS,
   and the main-module imports are not interchangeable across workers.
7. No new action on an Asyncify unwind/rewind path. Resume the original call and
   original module/snapshot before considering any compiler result.

The queue is not an architectural scheduler. PIT 1000/s, vblank 59.94/s,
APU 1500/s and the exact real-time Xbox TSC remain unchanged. Keep every
existing per-member IRQ/icount, `can_do_io`, breakpoint/plugin, GETPC/fault,
SMC, MMIO/watch and coherence check.

## 3. Request lifecycle

Use explicit states, independent of hotness counters:

`PREPARING → QUEUED → COMPILING → READY → INSTALLING → PUBLISHED`

Any nonpublished state can become `CANCELLED`, `STALE`, or `FAILED`.
Cancellation does not mean the engine stopped compiling. An active cancelled job
retains its in-flight permit until its Promise settles or termination is known.
Never spawn unlimited replacement jobs behind abandoned Promises.

A bounded request owns:

- session/worker identity, unique request ID, code-cache epoch;
- root identity and every proposed region member's identity;
- immutable bytes in a private ArrayBuffer (not a view of the shared heap);
- copied helper-index vector and immutable installation metadata;
- region member order, IC site count, instruction-count metadata;
- stage timestamps, byte count, status and failure/drop reason.

Submission copies synchronously before yielding or freeing C buffers. For
regions, copy the completed assembled module, not dangling `mod.p`, `hq`, or
stack arrays. For standalone TBs, do not leave an async operation reading the
recyclable code-cache blueprint/import vector. No speculative guest-byte fetch.

Session + request IDs prevent late callbacks from touching a replacement queue,
worker, VM session, or reused request slot. Handle ID wrap explicitly by draining
and advancing the session, or disabling async safely; never silently reuse IDs.

## 4. Lifetime protection: CF_INVALID alone is insufficient

`do_tb_phys_invalidate()` sets `CF_INVALID` under `tb->jmp_lock`.
`tb_gen_code()` can revive the same TB by clearing it under that lock.
`tb_flush__exclusive_or_serial()` resets the code region, reusing header addresses.
Thus pointer equality, guest PC, current CF_INVALID, and jump-cache generation
are not sufficient to reject a result invalidated and then revived/recycled
while compilation was outstanding. CF_PCREL/page invalidation matters too.

Preferred simple contract:

- Append an aligned 64-bit **validity incarnation** to `WasmTBHeader`, preserving
  all existing offsets. Current wasm32 header is 96 bytes; this would make it
  104 bytes. It is a real fixed 8-byte/TB cost, including when the flag is off;
  account for reduced TB-cache capacity and request approval for this choice.
- Give each newly published or revived valid TB a fresh nonzero incarnation.
  Clear/revoke it inside the same `jmp_lock` critical section that sets INVALID.
  Revival always gets a different incarnation, even for identical guest bytes.
- Snapshot incarnation + exact header/TB identity + code-cache epoch in requests.
- Advance/cancel the async cache epoch **before** region reset/reuse. Do not rely
  solely on the existing flush counter increment, which occurs after reset.
- A stale-epoch result is discarded without touching its saved raw pointers.
- At validation, resolve the live TB through the current code-region lookup,
  require exact `tb->tc.ptr`, valid cflags and matching incarnation, then perform
  the final publication checks under the lifecycle locks described below.

All new incarnation accesses need a consistent atomic/locking contract on
wasm32. No per-executed-TB generation update is required: only translation,
revival, invalidation, and compilation publication boundaries.

Alternative if the header cost is rejected: a bounded pending-membership
registry, registered and revoked under each TB's `jmp_lock`, with cancellation
that revival cannot undo. This avoids per-TB growth but needs a more intricate
stable-slot/ABA proof. A naked pointer-keyed JS Map is not that proof.

## 5. Transactional owner-thread publication

A safe point is a genuinely fresh dispatcher boundary after the previous
TCI/JIT call returned, with Asyncify in its normal state. It is not an arbitrary
JS callback, a call-site return during rewind, or merely `do_init != 0` observed
from another context. Preserve the existing one-suspended-activation/module
restriction. Existing pending C/TCI continuations must finish via their original
representation; late completion cannot convert their replay into a fresh JIT run.

Prefer a ready candidate whose root or member is the current fresh TB. Avoid
installing cold queued work merely to empty the queue. Shared-region entry through
that member must keep the existing member-index dispatch, not restart at the root.

For one selected ready candidate:

1. Check session, cache epoch, request status, live identities, and current
   ownership without dereferencing invalidated cache memory.
2. Require instance capacity. Do not make promise callbacks evict or exceed the
   cap; ready work can remain bounded or be dropped. Keep existing clock eviction
   and incoming-link clearing before index reuse at their established safe points.
3. Bind helpers from the **owner's current** `wasmTable` using copied indices.
   Allocate candidate ownership/member metadata before taking publication locks.
4. Instantiate and register an **unpublished** function on the owner thread.
   This is synchronous and can still stall. Make partial-install rollback explicit:
   remove a newly allocated table entry/JS record on error or cancellation; never
   leave an orphan index or half-published header.
5. Revalidate after instantiation. Acquire member `jmp_lock`s in deterministic
   order, preferably by trylock/defer; release partial acquisitions on failure.
   Verify epoch, incarnations, !INVALID, reservations and lack of live ownership
   again under these locks. Use nonblocking acquisition/release-and-defer rather
   than introducing a new multi-lock wait order. Invalidation/revival must use the
   same token protocol.
6. Publish fully initialized C ownership/member metadata as one owner-thread
   transaction. No JS, allocation, waiting for compilation, page-lock acquisition,
   or callback while holding this set of locks. The sole guest-execution thread
   cannot enter a partially published region.
7. Release locks, retire the request/reservations, then use the ordinary dispatcher
   entry and validated IC fill. On any failed check, clean up the unpublished
   function outside locks and continue TCI/existing code.

The publication linearization point is the final locked ownership commit:
invalidated before it → discard; invalidated after it → normal existing TB
invalidation/link revocation rules apply. Pre-install validation alone has a
TOCTOU hole. Never clear a newer request's reservation during old-result cleanup.

Full flush is already an exclusive/serial operation; serialize queue cancellation
with it before reset. Restore/reset/shutdown must also cancel prior-session work;
pending Modules/jobs are not part of guest snapshots. Do not change save/restore
or interrupt behavior to make compilation convenient.

## 6. Regions and links are part of the candidate

Split current `compile_region()` into preparation and publication, retaining
current selection/CFG/depth/byte limits and original member entry bodies.
Keep default shared regions: switching to asynchronous standalone-only code is
an exploratory fixture, not a comparable final performance candidate.

Reserve all selected members in a bounded pending-membership map. Update
`region_member_ok()`/submission logic so that overlapping pending jobs do not
fight over ownership. A member can keep running via TCI while reserved. Any
member invalidation, incarnation change, or competing live ownership makes the
entire prepared region stale; do not silently publish a subset of compiled bytes.

Snapshots retain all members, not just the root. Validate and publish all members.
Never steal an existing stable region or replace its snapshot globals. Changing
live guest successors still goes through the original generated target/ownership
checks and existing fallback routing.

IC=2 remains default; IC=1 can retain its existing mutable-IC rules. Instantiate
empty IC slots and fill only through the existing validated dispatcher path.
Do not snapshot reusable successor function indices at enqueue time.

Initial compatibility guard: legacy immutable `XEMU_WASM_LINK=1` and legacy
nonshared-region ownership can stay on the original synchronous pipeline with an
explicit reason counter. Do not silently turn those features off. Supporting
immutable links later requires revalidation/rebinding at publication and must not
invoke `compute_links()`'s current recursive synchronous child compilation from
an async preparation path.

## 7. TCI continuation and bounded servicing

A pending request must not repeatedly call the synchronous compiler or rebuild a
region every time the hot TB is entered. Likewise, queue-full hot TBs need a
bounded deferred/retry path, not per-entry JS Promise polling or allocation.

Extend the cold-chain decision to distinguish uncompiled, pending/deferred,
ready and installed states. Pending/deferred TBs can continue existing TCI
chaining. Arrange a bounded compiler-service return at a completed TB boundary
so that a long TCI chain cannot starve ready publication. The original entry
IRQ/icount guard still executes for **every** TB; compiler notification polling
is not guest IRQ/timer polling.

Use coarse owner maintenance opportunities for queue service, not new helper
imports in generated guest code, per-store clocks, or Promise checks at every
one of the ~26M TB transitions/s. Shared readiness flags are notification hints,
not proof that an owner-local Module has been delivered or that a TB is valid.
Do not overload INT32_MIN/INT32_MAX hotness sentinels with “compiling”. Saturate
hotness bookkeeping if prolonged TCI execution can otherwise overflow it.

Provisional resource limits (to be measured, not claimed optimal):

- one in-flight engine compile initially; try two only after CPU/RSS evidence;
- at most eight total queued/in-flight/ready requests;
- at most 2 MiB retained input bytes, at most 256 KiB per async module;
- at most one publication per bounded maintenance service, with additional
  host-side budget/credits to prevent repeated services draining a burst at once;
- no emergency synchronous compilation merely because the async queue is full.

Oversized/incompatible modules can use the original synchronous path, with
explicit counters. Failed valid jobs stay TCI with bounded backoff/quarantine;
avoid a failing async→sync→async retry loop. Infrastructure failure can disable
new async submissions and return to the original policy only at fresh boundaries.
Catch Promise rejections and identify invalid-module, infrastructure and capacity
failures separately. An uncatchable browser OOM is not a recoverable API guarantee.

Timeout is not cancellation: a slow/cancelled engine job still consumes its
permit until settled. Bound ready Modules by count too; compiled native code has
opaque memory cost outside the fixed 2048-MiB linear heap. Do not equate process
RSS with linear-heap headroom.

Budgeting: charge synchronous preparation/copy/instantiate/publish work to the
vCPU compile budget. Do not charge total asynchronous Promise latency as though
it were blocked vCPU time. Conversely, removing that debt must not open an
unbounded low-threshold compile flood: queue, byte and concurrency limits govern
background pressure independently. Keep phase/compile attribution honest.

## 8. Event-loop progress and worker options

### A. Owner-worker Promise (preferred feasibility prototype)

A synchronous submission EM_JS function calls `WebAssembly.compile(copiedBytes)`
and returns immediately. Its `.then/.catch` only populate the bounded ready map.
Do **not** use `EM_ASYNC_JS` to await the compilation: that parks the vCPU instead
of letting TCI run. Do not call a synchronous Module constructor afterward as a
supposed guaranteed cache hit.

V8 can use background compilation threads, but the API does not promise a
particular number of cores or that every lazy function is precompiled. No V8
flags or synthetic guest execution to warm it.

Observe submission, first available owner callback, and publication under actual
Mac workloads. In this mode the submit→callback duration includes engine work,
engine queueing AND time the owner could not process callbacks. It is not isolated
engine CPU time.

### B. Dedicated plain compiler worker (only if needed)

Transfer private byte buffers; compile there; postMessage the WebAssembly.Module
back by structured clone. Do not instantiate it there or transfer exported guest
functions. Avoid loading the whole emulator or allocating another Emscripten
pthread C/Asyncify stack merely to compile bytes. Respect CSP/worker URL deployment
and test Module cloning/features in the actual Chrome/browser build.

This isolates preparation of the Promise result from a blocked vCPU and permits
producer-vs-owner delivery telemetry (using comparable host timestamp origins,
not unrelated raw worker performance.now() readings), but still does not deliver
a Module into a busy owner without owner event-loop progress. An SAB completion word cannot carry
a Module object. Node-only synchronous MessagePort receive APIs are not a browser
solution. Engine-internal caching across isolates is not a portable substitute.

### If existing owner task opportunities are insufficient

Stop and review a separate scheduling amendment. A bounded **host task yield**
at an audited quiescent CPU boundary could let callbacks run without awaiting a
particular compile. A microtask-only yield may not service compiler/message tasks.
A sleep(0) can have timer clamping and holding the BQL can block devices; blindly
adding it inside `trysleep()` is not an approved solution. A MessageChannel-style
turn needs the correct worker, Asyncify path, lock/RCU audit and state revalidation
after resumption. No per-TB forced yield, nested guest stack, IRQ suppression,
clock catch-up shortcut, or mandatory browser flags.

This question is resolved **before** a performance implementation, not hidden in
an eventual benchmark. A helper worker alone does not remove it.

## 9. Diagnostic evidence and acceptance

Feature-level counters remain available with TB_STATS=0; no reintroduction of
always-on generated TB counters. Append fields/event summaries; record XEMU flags.
Collect bounded histograms/aggregates rather than unbounded per-TB logs:

- requested/duplicate/deferred jobs; queued/active/ready high water and bytes;
- submitted, finished, delivered, published, failed and timed out;
- stale/cancel reason: invalidation, revive, flush, ownership, session, capacity;
- preparation, copy/submission, callback latency, owner delivery (when measurable),
  instantiate and publication durations; P50/P95/P99/max;
- first real guest entry cost, fallback TCI exposure, queue residence and wasted
  completed work; distinguish latency, counts, bytes and sampled CPU time;
- live instances, GC/reclamation lag, linear heap and process/worker memory.

Validation before Mac A/B:

1. Actual-source lifecycle fixtures with a controllable compiler completion order:
   SMC during compile, invalidate→revive, CF_PCREL, flush→same-address reuse,
   duplicate/out-of-order completion, slot reuse/wrap and worker/session teardown.
2. Real generated modules: standalone, self-loop, direct/indirect IC and shared
   regions; deliberately overlap candidate members and revoke a non-root member.
   Assert no early ownership, no import from the wrong table, no stale IC target,
   and complete rollback after instantiate/register failures.
3. Prove event-loop/completion liveness without synthetic guest warmup; distinguish
   mock promises from actual WebAssembly.compile and actual browser behavior.
4. Force unwind before/after guest side effects, while TCI is pending, through IC
   tails and region members. Deliver results during a suspended continuation.
   Prove no prefix/store replay and no replacement/reuse of its snapshot module.
5. Queue full/byte cap/slow compile/failure/instance cap/eviction stress; no busy
   loop, permit leak, unbounded native work, or large ready-publication burst.
6. Intro/default-vs-on, save/restore, then SC/GR/R6 gameplay/new-effect runs on Mac.
   Every run retains real clock-drift checks (>1% flagged). Guarded local tests
   remain intro-only, TB64, serial, with the 5-GiB floor; no local R6/Chrome run.

Report frame-gap/frametime tails, first-walk/zoom stalls, steady FPS and loading
latency. Average FPS alone can hide the target problem. More TCI time, compiler
CPU contention, extra yields or delayed region ownership can regress performance.
Only the measured bundle can establish a win; no promised compile-stall elimination.

## Review decisions requested

- Approve the prepare/compile/owner-install/publish split and bounded TCI fallback.
- Approve default-region support and whole-region stale-result rejection.
- Choose the simple +8-byte header incarnation versus a more complex bounded
  pending-registry cancellation protocol before implementation.
- Treat owner event-loop liveness as the first gate. Any new host-task yield/lock
  scheduling behavior requires explicit follow-up review; clocks stay untouched.

No production files changed for this note. No async-JIT hash exists yet.
