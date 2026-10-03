#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Summarize XEMU_WASM_INSN_CENSUS=1 Mac reports (including *-hot.json).

The 64 masks partition entered TBs. Instruction weights are entry count times
static TB length, not retired instructions, REP iterations, or CPU-time shares.
Family totals overlap: an SSE+x87 TB contributes to both requirement families.
40 = proposal-v1 structural integer candidate; 41 = its no-memory subset.
Live segment/TLB checks are not measured here. Faults/early exits can overcount
instruction weights. Use this diagnostic to choose coverage, not to quote FPS.
"""
import argparse
import json
import math
from pathlib import Path

FAMILIES = ['sse', 'x87', 'mmx', 'system', 'string', 'other']


def summarize(document, window='last60s'):
    report = document.get('report', document)
    values = report.get('summary_'+window, report)
    if not any(k.startswith('n_census_') for k in values):
        raise ValueError('no census counters in selected window; enable '
                         'XEMU_WASM_INSN_CENSUS=1 before starting the emulator')

    def get(kind, bucket):
        value = values.get(f'n_census_{kind}_{bucket:02x}', 0)
        if isinstance(value, list):
            value = value[0]  # additive average rate, NEVER the median
        if value is None or not math.isfinite(value) or value < 0:
            raise ValueError('invalid counter; cannot form valid shares')
        return value

    totals = {k: sum(get(k, b) for b in range(64)) for k in ['tb', 'insn']}
    if not all(totals.values()):
        raise ValueError('no entered guest TBs/instruction weights in window')

    def share(buckets):
        return {k+'_pct': round(100 * sum(get(k, b) for b in buckets)
                               / totals[k], 4)
                for k in totals}

    exact = {}
    for mask in range(64):
        if get('tb', mask):
            label = '+'.join(name for i, name in enumerate(FAMILIES)
                             if mask & (1 << i)) or 'int-only'
            exact[f'{mask:02x}:{label}'] = share([mask])
    return {
        'version': 1,
        'basis': 'entered TBs / static TB-length weights, NOT CPU-time shares',
        'window': window,
        'totals_or_average_rates': totals,
        'exact_masks_partition': exact,
        'requirements_overlapping': {
            name: share([b for b in range(64) if b & (1 << i)])
            for i, name in enumerate(FAMILIES)
        },
        'candidate_integer_upper_bound': share([0x40]),
        'candidate_register_only': share([0x41]),
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
    except (ValueError, TypeError) as exc:
        parser.error(str(exc))
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
