#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Summarize XEMU_WASM_INSN_PROFILE=1 Mac stats reports (also *-hot.json).

Use this flag ALONE on the current baseline: it starts the normal phase sampler
without enabling hot TB counters or the instruction-count census. Generated
entry/helper-return/inline tags use the census's structural admission rules.
Explicit JIT_PROFILE subscopes are folded into their TB class; C helpers/slow
paths remain separate. No clocks, helpers, or counter increments are added to
translated instruction bodies beyond the opt-in phase stores.

Direct conditional/unconditional self-loop TBs are conservatively withheld from
savings. This catches the observed memory-poll shapes without game/PC matching,
but also excludes productive self-loops and does NOT identify every multi-TB or
indirect wait. A runtime self-edge adds loop attribution until a phase reset;
known direct self-loop TBs retain it across resets. Non-loop != productive.

Sampled wall-time attribution, not native cycles. Include the loop and unknown
budgets in denominators; capped R6 polling throughput is not an FPS forecast.
"""
import argparse
import json
import math
from pathlib import Path


CLASSES = ['eligible', 'eligible_loop', 'other', 'other_loop', 'unknown']


def summarize(document, window='last60s'):
    if not isinstance(document, dict):
        raise ValueError('report must be an object')
    report = document.get('report', document)
    if not isinstance(report, dict):
        raise ValueError('report must be an object')
    values = report.get('summary_' + window, report)
    if not isinstance(values, dict):
        raise ValueError('summary must be an object')

    def get(suffix):
        key = 'n_insn_sample_' + suffix
        value = values.get(key, 0)
        if isinstance(value, list):
            if not value:
                raise ValueError('empty sample summary: ' + key)
            value = value[0]  # additive mean; never median
        if (not isinstance(value, (int, float)) or isinstance(value, bool) or
                not math.isfinite(value) or value < 0):
            raise ValueError('invalid sample counter: ' + key)
        return value

    if not any(k.startswith('n_insn_sample_') for k in values):
        raise ValueError('enable XEMU_WASM_INSN_PROFILE=1 before startup '
                         'and capture gameplay stats: no class-time counters')
    total = get('total')
    if not total:
        raise ValueError('no sampler observations in selected window')
    samples = {k: get(k) for k in CLASSES}
    known = sum(samples[k] for k in CLASSES[:-1])
    unknown = samples['unknown']
    # Summary means round to one decimal; tolerate that, not corruption.
    if (any(v > total for v in samples.values()) or
            known + unknown > total + .35):
        raise ValueError('class samples exceed vCPU denominator')
    def pct(a, b):
        return round(100 * a / b, 4) if b else None

    eligible = samples['eligible']
    loops = samples['eligible_loop'] + samples['other_loop']
    return {
        'version': 1,
        'window': window,
        'basis': 'sampled vCPU phase wall time, totals or additive mean rates; '
                 'NOT instruction counts or native CPU cycles',
        'totals_or_average_rates': {'vcpu': total, **samples},
        'vcpu_pct': {
            'eligible_nonself': pct(eligible, total),
            'eligible_selfloop_withheld': pct(samples['eligible_loop'], total),
            'other_generated': pct(samples['other'], total),
            'other_selfloop': pct(samples['other_loop'], total),
            'unattributed_jit_or_bridge': pct(unknown, total),
            'helpers_dispatch_tci_etc': pct(max(0, total-known-unknown), total),
        },
        'eligible_nonself_pct_of_classified_jit': pct(eligible, known),
        'eligible_nonself_pct_including_unknown_denominator':
            pct(eligible, known + unknown),
        'all_candidate_pct_of_classified_jit_NOT_gain_share':
            pct(eligible + samples['eligible_loop'], known),
        'all_selfloop_pct_of_classified_jit': pct(loops, known),
        'conditional_speedup_pct': {
            str(r): round(100 * (1 / (1 - eligible / total * r / 100) - 1), 4)
            for r in [20, 30]
        },
        'caveats': [
            'Speedup scenarios assume eligible nonself time is productive '
            'critical-path time and 20/30% of it is removed, without added '
            'overhead. Neither assumption is measured here.',
            'Self-loops are withheld, not proved idle. Unrecognized waiting '
            'may remain in the nonself bucket.',
            'Class time includes mandatory guard/entry/exit glue; it is not '
            'all removable. Unknown includes bridge/prologue/startup time '
            'and is not credited as eligible.',
            'Instrumentation and scheduling can perturb sampled shares; use '
            'the same baseline and route, and compare an uninstrumented run.',
            'Capped FPS does not establish useful-work headroom.',
        ],
        'environment': report.get('env', report.get('xemu_env')),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('report', type=Path)
    parser.add_argument('--window', choices=['last60s', 'all'],
                        default='last60s')
    args = parser.parse_args()
    try:
        result = summarize(json.loads(args.report.read_text()), args.window)
    except (ValueError, TypeError, IndexError) as exc:
        parser.error(str(exc))
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
