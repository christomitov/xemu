#!/usr/bin/env python3
"""One exclusive benchmark session containing two simultaneous guarded arms."""
import fcntl
import importlib.util
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time

spec = importlib.util.spec_from_file_location('guard', '/tmp/astra-runtime-guard.py')
guard = importlib.util.module_from_spec(spec)
spec.loader.exec_module(guard)
root = Path('/root/src/xemu-wasm')
name, option, *common = sys.argv[1:]
assert option.startswith('XEMU_') and '=' not in option
marker = root / '.integrator-building'
if marker.exists():
    sys.exit('BUILD_BUSY')
guard.check()
lock = open(root / '.node-run.lock', 'w')
fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
guard.check()
if marker.exists():
    sys.exit('BUILD_BUSY')
base = Path('/tmp/astra-lock-pair-' + name)
monitor = open(str(base) + '-rss.jsonl', 'x')
children = []
order = [1, 0] if os.environ.get('ASTRA_REVERSE') == '1' else [0, 1]
t0 = time.monotonic()
reason = 'exit'

def interrupted(signum, frame):
    raise KeyboardInterrupt
signal.signal(signal.SIGTERM, interrupted)
try:
    for arm in order:
        pfx = str(base) + '-' + str(arm)
        result = pfx + '.json'
        assert not Path(result).exists()
        log = open(pfx + '.log', 'x')
        env = dict(os.environ, BENCH_JS=str(root / 'xemu-astra/build/qemu-system-i386.js'),
                   BENCH_STATE=str(root / 'bench/sc-gameplay.state'),
                   MALLOC_ARENA_MAX='2', BENCH_TRACE='1', ASTRA_ROWS=result)
        args = [*common, option + '=' + str(arm)]
        for arg in args:
            k, sep, v = arg.partition('=')
            if sep and k.startswith('XEMU_'):
                env[k] = v
        proc = subprocess.Popen(['nice', '-n', '10', 'node',
            '--experimental-wasm-branch-hinting', '/tmp/astra-lock-bench.mjs',
            'r6snap', os.environ.get('ASTRA_SECS', '60'), *args],
            cwd=root, env=env, stdout=log, stderr=subprocess.STDOUT,
            start_new_session=True)
        children.append(dict(arm=arm, proc=proc, log=log, result=result,
                             start=time.monotonic()-t0))
    while True:
        entry = dict(t=time.monotonic()-t0, available=guard.available_kib(),
                     load=float(Path('/proc/loadavg').read_text().split()[0]),
                     arms=[])
        rss_total = 0
        for c in children:
            p = c['proc']
            rss = 0
            try:
                for s in Path(f'/proc/{p.pid}/status').read_text().splitlines():
                    if s.startswith('VmRSS:'):
                        rss = int(s.split()[1])
            except FileNotFoundError:
                pass
            rss_total += rss
            entry['arms'].append(dict(arm=c['arm'], pid=p.pid, rss=rss,
                                      rc=p.poll()))
        monitor.write(json.dumps(entry)+'\n'); monitor.flush()
        if entry['available'] < guard.FLOOR_KIB:
            reason = 'MEMAVAILABLE_GUARD_5GiB'; break
        # Joint RSS cap is deliberately stricter than a separate cap per arm.
        if rss_total > 7.5*1024*1024:
            reason = 'COMBINED_RSS_GUARD_7.5GiB'; break
        if all(Path(c['result']).exists() for c in children):
            reason = 'final_JSON'; break
        if any(c['proc'].poll() is not None and not Path(c['result']).exists()
               for c in children):
            reason = 'arm_failed'; break
        if entry['t'] > 200:
            reason = 'TIMEOUT'; break
        time.sleep(1)
finally:
    for c in children:
        p = c['proc']
        if p.poll() is None:
            if reason == 'final_JSON':
                try: p.wait(timeout=2)
                except subprocess.TimeoutExpired: pass
            if p.poll() is None:
                os.killpg(p.pid, signal.SIGTERM)
                try: p.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    os.killpg(p.pid, signal.SIGKILL); p.wait()
        c['log'].close()
    monitor.close()
    outcome = dict(reason=reason, elapsed=time.monotonic()-t0,
        option=option, arms=[dict(arm=c['arm'], pid=c['proc'].pid,
        rc=c['proc'].returncode, start=c['start'], result=c['result']) for c in children])
    Path(str(base)+'-outcome.json').write_text(json.dumps(outcome,indent=2))
    print(json.dumps(outcome))
if reason != 'final_JSON' or any(c['proc'].returncode for c in children):
    sys.exit(1)
