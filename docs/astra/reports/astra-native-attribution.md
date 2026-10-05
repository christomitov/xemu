# ASTRA: vCPU native attribution — report only

## Result

The large native consumer is exact host-clock reading, predominantly guest RDTSC.
This Linux host uses **HPET**, not a userspace TSC clocksource:
`/sys/devices/system/clocksource/clocksource0/current_clocksource` reads `hpet`.
The vDSO falls back to the `clock_gettime` syscall. Kernel `read_hpet` alone
accounts for **7.98%** of sampled vCPU on-CPU time.

Fresh SC gameplay capture on clean `8b8b3ce0b6` (perf-13 + deadline cleanup):

| Identifiable native consumer | Samples / 35,602 | % vCPU on-CPU samples | Wasm caller |
|---|---:|---:|---|
| Host clock reads, userspace + kernel | 4,364 | **12.258%** | Primarily `helper_rdtsc -> cpu_get_tsc -> qemu_clock_get_ns -> cpus_get_virtual_clock -> cpu_get_clock -> xemu_wasm_now_ms`; also `bql_lock_impl -> xemu_wasm_stats_now_ns -> __clock_gettime -> _clock_time_get` |
| Wasm compile / instantiate / lazy compilation / tiering | 272 | **0.764%** | `compile_tb` / `cpu_tb_exec -> wasm32_instantiate`, or first execution of a generated Wasm function through `Runtime_WasmCompileLazy` |
| Exception / longjmp runtime | 211 | **0.593%** | Mostly `cpu_loop_exit_restore -> ... -> emscripten_longjmp -> __emscripten_throw_longjmp`, then V8 exception search / stack walking |

Percentages are native/kernel self samples grouped by their captured callers,
not inclusive time inside arbitrary Wasm parents. JS glue, import stubs and
ordinary Wasm execution are not included in these native totals. Small-tail
rankings are approximate and depend on grouping; this is not an optimization
A/B or a claim that these costs are removable.

### Clock breakdown

- **10.994%** of all vCPU samples: clock-native stacks containing `helper_rdtsc`.
- **1.003%**: clock-native stacks containing `xemu_wasm_stats_now_ns` and
  `bql_lock_impl`. This is clock-reading CPU cost, **not BQL waiting time**.
- Other stats timing 0.143%, MMIO timing 0.045%, other/truncated clocks 0.073%.
- `read_hpet` self 7.980%; vDSO `clock_gettime` self 1.907%; syscall dispatch,
  seccomp, timespec copy-out, libc and Node wrappers account for the rest.
- vDSO sampled PC `+0xd40` is immediately after `syscall` at `+0xd3e`.
  This explains why the V8 signal profiler charges kernel clock work to a
  userspace clock/native callback: signal delivery occurs after syscall return.
- The first perf capture was user-only (`task-clock:u`). It omitted this kernel
  cost and therefore is **not** the denominator used for the final report.
- `/proc` user/system deltas in the second capture show approximately 11.85%
  system CPU; perf samples show 10.20% kernel. Profiling/interrupt accounting and
  slightly different windows prevent treating those as identical measures.

### Remaining hypotheses

- Native bulk copy/fill self time: **0.194%** (69 samples), including V8
  `memory_copy_wrapper` / `memory_fill_wrapper` and libc memmove/memset.
  This is not a claim about inline memory work in generated Wasm.
- Futex / mutex / notify native CPU: **0.239%**. This excludes off-CPU sleep and
  does not include Wasm BQL retry instructions.
- Wasm table get/set: **0.317%**, principally `wasm32_ic_fill -> table.set`, with
  `cpu_tb_exec` below it. JS Map lookup self: **0.228%**, mainly the dispatcher.
  Neither is a large native slow-path bucket.
- Native stacks containing GC: **0.017%** (6 samples); too few for a precise
  estimate, but no evidence of a large vCPU GC consumer.
- Compile group separates into **0.447%** via `wasm32_instantiate`, **0.278%**
  lazy first-use compilation, **0.039%** other compilation/tiering. The first
  agrees with the coordinator's direct ~0.5% compile/install counter.
- Other native work totals **0.980%**, spread across IC/global/property loads,
  value conversion, small API operations, etc.; no additional single large
  consumer was established. Some are clock-call setup/return work outside the
  strict clock-native group above.
- Native + kernel total: **15.552%**. Generated wasm-to-JS stub self: **0.281%**.
  Do not force these fresh, current-build numbers to equal the old 13.5% Node /
  6.4% libc / 2.3% bridge buckets. That older raw capture is unavailable; it also
  predates the shipped UNWIND_MEM baseline used here.

## Measurement and symbolization

Three guarded SC60 captures completed with final benchmark JSON, rc0 and valid
PIT/vblank/APU clocks. No Mac jobs or project-source changes were made.

1. V8 `--prof --no-prof-browser-mode --interpreted-frames-native-stack`:
   PID 3586983, TID 3586992, isolate `0x73533c002000`, worker threadId 1.
   Corrected 21–59s window: 30,169 ticks; `FastPerformanceNow` attribution
   3,374 / **11.184%**. Raw-PC validation places 3,324 of those ticks in the
   vDSO, 32 in libc, 17 in Node and 1 in generated code. This independently
   agrees on the dominant caller but is not a separate CPU-time counter.
2. Perf user-only capture: retained as diagnostic evidence of the missing
   kernel component, not used for final CPU percentages.
3. **Authoritative perf capture:** PID 3626074, TID **3626082**, worker threadId 1.
   `task-clock`, 1,000,003ns period, **user + kernel**, sampled user registers
   and 8KiB user stacks. Attached approximately 20–58s after process launch;
   first two seconds of attachment excluded to stay safely past warmup.
   **35,602 analyzed samples, zero lost records**, all from that one TID.
   Timed JIT symbols plus captured stack chains establish this is the vCPU
   executing `cpu_exec_loop`, not merely a thread named `node`.

Native and kernel frames were mapped from actual PCs, ELF-sized symbol ranges,
recorded mapping offsets, matching libc debug symbols, vDSO dynamic symbols,
`/proc/kallsyms`, and timestamped V8 jitdump code-load records. Bounded frame-
pointer walks use only the captured stack; 5,536 / 5,537 native/kernel samples
have a resolved JIT ancestor. Unknown JIT/PC samples: 66 (0.185%); unresolved
native self symbols: 19 (0.053%, often with resolved callers). No unknowns were
renormalized out of the denominator.

`perf report`'s ordinary processing hit two 300s offline guards, so final
analysis uses the retained raw perf records with an explicit sample-ABI parser,
not those incomplete reports. No runtime capture was guard-killed.

V8 offline processing required two independent corrections:
- Its bundled `nm` subprocess default output limit is 1MiB; this Node symbol
  output is 15.7MiB. Private preload raises the limit to 128MiB.
- Node is ET_EXEC. Its logged `/usr/bin/node` mappings beginning at `0x400000`
  otherwise cause an erroneous +4MiB symbol relocation. A derived log changes
  only the final slide field on those headers to `4194304`. Original raw log is
  retained. `vcpu-symbolized.json` is the earlier **wrong-address** result and
  must not be used; `vcpu-corrected.json` is the corrected one.

Binary identities:
- Node v22.21.0: build ID `d54bda2638f9fdf646164b6bbaf9d49ed283f220`.
- libc: build ID `ae7440bbdce614e0e79280c3b2e45b1df44e639c`.

Source anchors on 8b8b3ce0b6:
- `target/i386/tcg/misc_helper.c:68` (`helper_rdtsc`).
- `hw/i386/x86-cpu.c:35` (`cpu_get_tsc`).
- `include/qemu/timer.h:859` (Wasm host-clock import).
- `ui/xemu-wasm.c:36` (`xemu_wasm_now_ms`), `:678` (stats clock).
- `system/cpus.c:611,620` (BQL wait timestamp calls).
- `accel/tcg/cpu-exec-common.c:76` (`cpu_loop_exit_restore`).

## Artifacts

- `/tmp/astra-native-prof-1/`: raw/corrected V8 logs and corrected JSON.
- `/tmp/astra-native-perf-{1,2}/`: perf.data, jitdump, identity, record logs.
- `/tmp/astra-native-perf-2/native-self-analysis.json`: per-self-function counts
  and caller chains; `native-groups.json`: purpose-group totals.
- `/tmp/astra-native-perf-2/{kallsyms.txt,clocksource.txt}`.
- `/tmp/astra-lock-{native-prof-1,native-perf-1,native-perf-2}*`: benchmark JSON,
  outcomes, resource samples and mapping snapshots.
- `/tmp/astra-perf-native-report.py`, `/tmp/astra-native-group-report.py`.
- `/tmp/astra-prof-symbols.cjs`, `/tmp/astra-native-nm.py`,
  `/tmp/astra-native-{tag.cjs,perf-run.py,offline.sh}` and `/tmp/astra-v8tag.cc`.
- Private downloaded tools/debug symbols under `/tmp/astra-native-symbols/`;
  no system package installation or host clocksource change.

## Decision / portability

This is an attribution report, **not authorization to cache/approximate RDTSC,
batch IRQs, weaken BQL or change the host clocksource**. HPET/syscall cost is a
property of this Linux host; do not assume a 12% clock budget exists on Mac.
The coordinator's Mac result for 8b8b3ce0b6 is NEUTRAL; it remains a dead-work
cleanup, untagged and off the performance graph. Current source stays clean at
8b8b3ce0b6. Translator/spin work remains closed.
