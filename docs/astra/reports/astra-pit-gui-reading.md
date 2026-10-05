# PIT/GUI report-only audit (closed; no changes)

PIT: SC saved-state i8254 v3 channel0 has mode2/count1125. Actual-source timing formulas produce low, then high~889ns later; ~999110ns to next low. `n_pit_tick` counts only low-to-high. `pic_set_irq` must see low to clear last_irr and detect another rising edge; XBOX also clears IRR on deassert. Both callbacks are functional, not duplicate dispatch. Existing i8254.c comment documents prior short-delay coalescing reducing IRQs to~350/s. Keep both edges. /tmp/astra-pit-transition-audit.json.

GUI: measured33ms/s (~0.55ms/vblank) was Node's NULL renderer with instrumentation, not Mac WebGPU. `nv2a_vga_gfx_update` includes generic VGA console/surface/dirty-bitmap work plus `xemu_wasm_fb_update` live-VRAM MEMFS export. WebGPU already bypasses both by default. No measured breakdown of these subparts; do not assign all0.55ms to any one operation. Headless benchmark overstates main-loop BQL cost relative to WebGPU Mac.

BQL-required/serialized parts: PCRTC pending interrupts/raster, multi-step PMC interrupt recomputation and PCI/PIC IRQ, mutable console/VGA/device lifecycle, active disc/QMP/SMC service and reset requests, timer rearm/resync. VGA dirty snapshot/clear may synchronize with the vCPU; do not unlock around it speculatively.

Potentially host-only work is not automatically safe outside BQL: MEMFS output currently consumes live VRAM/scanout state, not an owned snapshot. Present request itself only sets sync_pending under PFIFO lock and kicks the worker, but g_nv2a/renderer lifetime, reset/quiescence and lock order still require audit before moving it. No change authorized or made.

Sources: hw/timer/i8254{,_common}.c, hw/intc/i8259.c, ui/xemu-wasm.c:1030, ui/console.c:136, hw/xbox/nv2a/nv2a.c:207, hw/xbox/nv2a/xemu-wasm-fb.c:16, hw/xbox/nv2a/pgraph/pgraph.c:435.

User accepted conclusions, closed PIT/GUI work, and assigned native vCPU profile attribution. No ASTRA Mac jobs while coordinator's deadline A/B runs.
