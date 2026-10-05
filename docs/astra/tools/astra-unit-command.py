#!/usr/bin/env python3
# Used only inside the caller's .node-run.lock + build-marker critical section.
import importlib.util, os, pathlib, signal, subprocess, sys, time
spec = importlib.util.spec_from_file_location('guard', '/tmp/astra-runtime-guard.py')
g = importlib.util.module_from_spec(spec); spec.loader.exec_module(g)
g.check()
p = subprocess.Popen(['nice', '-n', '15', *sys.argv[1:]], start_new_session=True)
def stop(signum, frame): raise KeyboardInterrupt
signal.signal(signal.SIGTERM, stop)
started = time.monotonic()
timeout = int(os.environ.get('XEMU_WASM_UNIT_TIMEOUT_SEC', '300'))
try:
    while p.poll() is None:
        if time.monotonic() - started > timeout:
            raise RuntimeError('unit wall-clock guard')
        try:
            status = pathlib.Path(f'/proc/{p.pid}/status').read_text()
        except FileNotFoundError:
            break
        # Include compiler/Node children in our own process group, not just
        # the small Python/shell launcher. Counting shared pages twice is a
        # conservative guard; never inspect/terminate another owner's group.
        rows = subprocess.check_output(['ps', '-eo', 'pgid=,rss='], text=True)
        rss = sum(int(row.split()[1]) for row in rows.splitlines()
                  if int(row.split()[0]) == p.pid)
        if rss > 7.5 * 1024 * 1024 or g.available_kib() < g.FLOOR_KIB:
            raise RuntimeError('unit memory safety guard')
        time.sleep(1)
    rc = p.wait()
finally:
    if p.poll() is None:
        os.killpg(p.pid, signal.SIGTERM)
        try: p.wait(timeout=3)
        except subprocess.TimeoutExpired: os.killpg(p.pid, signal.SIGKILL); p.wait()
sys.exit(rc)
