# ASTRA wrap-up — 2026-10-05

**Stopped at the user's request. No new experiments, builds, emulator runs or
Mac jobs are authorized by these archival notes.** Historical proposals are
not outstanding implementation instructions.

## Current decisions

| Branch | Last code commit (before this docs-only archive) | Decision |
|---|---|---|
| `astra-gvec-sat16` | `74fa6fde1278d1a738e6335e8ddd14056a185a1c` | **Rejected**: -1.73% mean flips/CPU-second in two perf-14 SC pairs, despite eliminating the observed original-helper calls. Keep off/unintegrated. |
| `astra-vcpu-lock-census` | `55f2bba510` | Diagnostic-only census/subphase profiler; do not ship. |
| `astra-bql-debug-deadline` | `8b8b3ce0b6` | Integrated dead-work cleanup; **Mac neutral**, no performance tag/graph claim. |
| `astra-bql-spin-rejected` | `ec407e8ad8` | Rejected BQL-spin experiment; reverted on the diagnostic line. |
| `astra-direct-memory-miss-classify` | `3f7259894b` | Direct translator shelved after Mac gameplay regressions. |

Perf-14 is `65b75d638484566f14ded4ad67c5ec8644de27e0` (LIVE_SAVE default-on).
The coordinator reported SC/GR Mac wins and matching R6 boot+mission-load
behavior. ASTRA's earlier LIVE_SAVE review was source-only; later sat16
fixtures exercise real standalone Asyncify unwind/re-yield, not full-emulator
shared-region/foreign-instance qualification.

SIMD/v128 operations and values inside TB functions **may** inhibit TurboFan
optimizations or add spills, offsetting removed helper calls. This is an
unverified explanation for sat16's regression, not a native-profile result;
the candidate added no explicit v128 locals. Do not restart the experiment
or broaden it to mul16/pmaddwd based on that hypothesis.

## Start reading here

- [Sat16 result, gates and negative A/B](reports/astra-gvec-sat16-result.md)
- [Current handoff and historical lock results](reports/astra-vcpu-lock-status.md)
- [Native attribution: HPET host-clock path, not a portable Mac budget](reports/astra-native-attribution.md)
- [LIVE_SAVE adversarial source review](reports/astra-live-save-review.md)
- [PIT/GUI audit](reports/astra-pit-gui-reading.md)
- [GR handoff: not started](gr-handoff.md)
- [Direct translator rejection](reports/astra-direct-mixed-gameplay-decision.md)

## Archive layout and limits

- `reports/`: verbatim copies of ASTRA Markdown reports/designs from `/tmp`.
  Their absolute scratch paths and historical pending/approved language
  describe their original time; the decisions above supersede them.
- `evidence/`: selected machine-readable gates, final benchmark rows/outcomes,
  resource monitors, profile summaries and final test/build logs. These are
  **existing results**, not new wrap-up runs.
- `tools/`: diagnostic/report scripts retained for provenance. They contain
  server-local paths and assumptions; they are not portable production tools.
- `parked/`: existing unimplemented/parked patches, archived but not applied.
- `source-branches.json`: local ASTRA branch tips before the documentation
  commits. Source branches retain their original experiment histories.
- `archive-manifest.json`: source scratch path, archived destination, byte
  count and SHA256 for copied artifacts.
- `root-isolate-cleanup.json`: manifest of the explicitly authorized deletion
  of ASTRA's untracked root-level V8 logs (PIDs3602964/3626074).

Large raw perf.data/jitdump files, executable binaries, generated build
products, screenshots and save-state images are not added to git. Derived
reports and their attribution/coverage limitations are preserved here. Some
historical `/tmp` artifacts were already pruned; do not assume every path in
an older report still exists. No unrelated worktree edits, other agents'
branches/stashes, `.claude/`, build catalogs or performance graphs were
changed during this wrap-up.
