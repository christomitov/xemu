# GR follow-up — stopped before implementation

The user requested a headless Ghost Recon gameplay state and vCPU profile,
then explicitly stopped the task before implementation.

## What actually happened

- Read `tools/bench.mjs` and the `gr` preset in `tools/queue-job.mjs`.
- Verified the current production build is perf-14 (`65b75d6384`).
- Preserved a scratch preimage of `tools/bench.mjs`; **made no changes to it**.
  Its SC scenario, per-PID overlay and CPU metrics belong to the coordinator
  and were committed in outer-repo `80d3ea9`.
- No GR scenario, GR state file, GR branch, emulator run, profile or build was
  created. No Mac job was started. Existing `bench/sc-gameplay.state` retained.
- Added the requested TurboFan/spill hypothesis to the archived sat16 report,
  explicitly marked as unverified and noting no explicit v128 locals added.

## Context for a future, separately authorized task

The user reported SC29.4 and R629.5 at the cap after perf-14; GR26.4 remained
below it, with78% of seconds below28FPS. The coordinator started a Mac
PROFILE=1 GR job to determine whether it is vCPU- or GPU-bound. No result was
received or inferred here.

Mac `gr` preset, read-only observation:
- Second entry on the Rainbow Six3 demo DVD.
- Down at t33; A/Space at t36; Start/Enter once at t64.
- From t67, A every2 seconds until draws>=2000/s and FPS<=35, bounded by t160.
- Start in the later menus goes to a return menu; do not keep pressing it.

Important headless caveat: `n_draw` increments in the WebGPU renderer, not
the NULL renderer. Blindly copying the Mac until-draws predicate would never
stop confirming. A headless gameplay detector needs qualification (e.g.
heavy PGRAPH activity per flip plus an appropriate frame-rate limit), not a
claim that method counts themselves prove the correct gameplay scene.

Existing bench code already has BENCH_SAVE_AT/BENCH_SAVE_TO and BENCH_STATE,
but a future GR implementation should ensure save completion/failure is
reflected in the final result and suppress menu/reset inputs on state reload.
Capture into a temporary filename, qualify a reload and guest progress, then
publish `bench/gr-gameplay.state`. These are suggestions, **not implemented
or tested changes**.

If later authorized to profile, use the exact perf-14-or-approved successor
build, full sampling denominator, explicit vCPU TID/isolate tagging, and
user+kernel perf samples: this Linux host uses HPET, so userspace-only samples
miss a large host-clock cost. Do not infer Mac bottlenecks from NULL-renderer
headless timing. Retain the5GiB available-memory/7.5GiB RSS guards, load<6 at
start, and shared Node/build ownership rules. New Mac work remains solely
coordinator-owned.
