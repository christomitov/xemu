Direct Wasm translator: parked
==============================

The direct translator remains opt-in. Do not promote it or expand its
instruction coverage without a new, explicit investigation.

Mixed-region build 02d8139751 regressed matched Mac gameplay: GR 22.3/23.0
versus 25.4/26.6 FPS, SC 23.3/23.3 versus the valid 28.5 FPS control, and
R6 28.5 versus 29.5 FPS. The second SC control went off-route and is excluded.
The intro improvement did not transfer to gameplay.

Miss diagnostics are parked at 1384ab20c1. In one 60-second saved-state SC
headless run, t20--60 averaged 8243 canonical misses/s: 38.7% aligned exact
NOTDIRTY store tags, 25.0% alignment, 25.0% page mismatch, 11.4% other flags,
and zero unknown. Tags were captured from the original probe, not reread.
An exact NOTDIRTY tag does not establish fast-leaf eligibility or cost.

The diagnostic fixtures and private build passed. No NOTDIRTY optimization,
dirty-mask optimization, or admission-policy change was implemented. The
coordinator ended the investigation after classification; further translator
work is shelved in favor of profile-grounded legacy-backend work.
