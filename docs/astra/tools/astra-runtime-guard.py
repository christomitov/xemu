#!/usr/bin/env python3
import pathlib, subprocess

FLOOR_KIB = 5 * 1024 * 1024

def available_kib():
    for line in pathlib.Path('/proc/meminfo').read_text().splitlines():
        if line.startswith('MemAvailable:'):
            return int(line.split()[1])
    raise RuntimeError('Missing MemAvailable')

def emulator_nodes():
    pids = subprocess.run(['pgrep', '-x', 'node'], capture_output=True, text=True).stdout.split()
    matches = []
    needles = ('bench.mjs', '/tmp/astra-', '/tmp/test-wasm-', '/tmp/test-tci-',
               'tci-direct-call', 'qemu-system-', '/xemu-wasm/tools/', '/xemu-astra/tests/')
    for pid in pids:
        try:
            cmd = pathlib.Path(f'/proc/{pid}/cmdline').read_bytes().replace(b'\0', b' ').decode(errors='replace')
        except FileNotFoundError:
            continue
        if any(s in cmd for s in needles):
            matches.append((pid, cmd))
    return matches

def check():
    busy = emulator_nodes()
    if busy:
        raise SystemExit(f'EMULATOR_NODE_BUSY {busy}')
    if available_kib() < FLOOR_KIB:
        raise SystemExit('MEMAVAILABLE_BELOW_5GiB')
    if float(pathlib.Path('/proc/loadavg').read_text().split()[0]) >= 6:
        raise SystemExit('LOAD_BUSY')

if __name__ == '__main__':
    check()
