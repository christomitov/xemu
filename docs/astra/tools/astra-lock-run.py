#!/usr/bin/env python3
import fcntl, json, os, pathlib, shlex, signal, subprocess, sys, time
import importlib.util
spec = importlib.util.spec_from_file_location('guard', '/tmp/astra-runtime-guard.py')
guard = importlib.util.module_from_spec(spec); spec.loader.exec_module(guard)
root = pathlib.Path('/root/src/xemu-wasm')
name, *args = sys.argv[1:]
if (root / '.integrator-building').exists():
    sys.exit('BUILD_BUSY')
guard.check()
lock = open(root / '.node-run.lock', 'w')
fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
guard.check()
if (root / '.integrator-building').exists():
    sys.exit('BUILD_BUSY')
prefix = pathlib.Path('/tmp/astra-lock-' + name)
log = open(str(prefix) + '.log', 'w')
monitor = open(str(prefix) + '-rss.jsonl', 'w')
result = str(prefix) + '.json'
assert not pathlib.Path(result).exists()
env = dict(os.environ, BENCH_JS=os.environ.get('BENCH_JS', str(root / 'xemu-astra/build/qemu-system-i386.js')),
           BENCH_STATE=os.environ.get('BENCH_STATE', str(root / 'bench/sc-gameplay.state')),
           MALLOC_ARENA_MAX='2', BENCH_VERBOSE='1', BENCH_TRACE='1', ASTRA_ROWS=result,
           ASTRA_MEM=str(prefix) + '-stats.jsonl')
# Keep OS-level metadata and Emscripten settings identical. Explicit '=0'
# and resource overrides are diagnostic settings too, not default runs.
for arg in args:
    key, sep, value = arg.partition('=')
    if sep and key.startswith('XEMU_'):
        env[key] = value
if os.environ.get('ASTRA_PRELOAD'):
    env['LD_PRELOAD'] = os.environ['ASTRA_PRELOAD']
p = subprocess.Popen(['nice', '-n', '10', 'node', '--experimental-wasm-branch-hinting',
                      *shlex.split(os.environ.get('ASTRA_NODE_ARGS', '')),
                      '/tmp/astra-lock-bench.mjs', 'r6snap', os.environ.get('ASTRA_SECS', '60'), *args],
                     cwd=root, env=env, stdout=log, stderr=subprocess.STDOUT,
                     start_new_session=True)
def interrupted(signum, frame):
    raise KeyboardInterrupt
signal.signal(signal.SIGTERM, interrupted)
t0 = time.monotonic()
reason = 'exit'
try:
    while p.poll() is None:
        now = time.monotonic() - t0
        d = dict(t=round(now, 2), pid=p.pid,
                 load=float(pathlib.Path('/proc/loadavg').read_text().split()[0]))
        try: status = pathlib.Path(f'/proc/{p.pid}/status').read_text()
        except FileNotFoundError: break
        for row in status.splitlines():
            k, _, v = row.partition(':')
            if k in ['VmRSS', 'VmSize', 'VmSwap', 'RssAnon', 'RssFile']:
                d[k] = int(v.split()[0])
        if int(now) % 5 == 0:
            tasks = []
            for task in pathlib.Path(f'/proc/{p.pid}/task').iterdir():
                try:
                    raw = (task / 'stat').read_text(); fields = raw.rsplit(')', 1)[1].split()
                    tasks.append([int(task.name), (task / 'comm').read_text().strip(), int(fields[11]) + int(fields[12])])
                except FileNotFoundError: pass
            d['tasks'] = tasks
        if int(now) in [30, 55]:
            pathlib.Path(str(prefix) + f'-smaps-{int(now)}.txt').write_text(pathlib.Path(f'/proc/{p.pid}/smaps').read_text())
        d['MemAvailable'] = guard.available_kib()
        monitor.write(json.dumps(d) + '\n'); monitor.flush()
        if d.get('VmRSS', 0) > 7.5 * 1024 * 1024:
            reason = 'RSS_GUARD_7.5GiB'; break
        if d['MemAvailable'] < guard.FLOOR_KIB:
            reason = 'MEMAVAILABLE_GUARD_5GiB'; break
        if pathlib.Path(result).exists():
            reason = 'final_JSON'; time.sleep(1); break
        if now > 200:
            reason = 'TIMEOUT'; break
        time.sleep(1)
finally:
    if p.poll() is None:
        os.killpg(p.pid, signal.SIGTERM)
        try: p.wait(timeout=5)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL); p.wait()
    monitor.close(); log.close()
    if pathlib.Path(result).exists():
        reason = 'final_JSON'
    outcome = dict(reason=reason, rc=p.returncode,
                   elapsed=round(time.monotonic() - t0, 2), pid=p.pid)
    pathlib.Path(str(prefix) + '-outcome.json').write_text(json.dumps(outcome))
    print(json.dumps(outcome))
if reason != 'final_JSON': sys.exit(1)
