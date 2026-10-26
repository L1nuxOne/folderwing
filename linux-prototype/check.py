#!/usr/bin/env python3
"""Build, audit and run the scoped Linux browser acceptance lane; seal evidence."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import signal
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]

def digest(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(',', ':')).encode()).hexdigest()

def command(args):
    return subprocess.check_output(args, cwd=ROOT, text=True).strip()

def source_snapshot():
    paths = subprocess.check_output(['git', 'ls-files', '-z', '--cached', '--others', '--exclude-standard'], cwd=ROOT).split(b'\0')
    return digest({os.fsdecode(p): {'sha256': hashlib.sha256((ROOT/os.fsdecode(p)).read_bytes()).hexdigest(),
                                  'executable': bool((ROOT/os.fsdecode(p)).stat().st_mode & 0o111)}
                   for p in sorted(set(paths)) if p and (ROOT/os.fsdecode(p)).is_file()})

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--backend', choices=['x11', 'wayland'], required=True)
    args = parser.parse_args()
    evidence = ROOT/'build/linux-prototype'/('check-'+args.backend)
    evidence.mkdir(parents=True, exist_ok=True)
    changes = ['linux-prototype/model.c', 'linux-prototype/model.h', 'linux-prototype/browser.c',
               'linux-prototype/desktop.c', 'linux-prototype/desktop.h',
               'linux-prototype/build.sh', 'linux-prototype/check.py', 'linux-prototype/operations.c', 'linux-prototype/operations.h', 'tests/fixtures/linux_ops_faults.c', 'tests/linux_browser_tests.c']
    for name, tail in [('audit', ['audit']), ('self-check', ['self-check']), ('selection', ['select', '--changed', *changes])]:
        result = subprocess.run([sys.executable, 'tools/native-impact.py', *tail], cwd=ROOT, capture_output=True, text=True)
        (evidence/(name+'.json')).write_text(result.stdout)
        if result.returncode:
            print(result.stderr, file=sys.stderr)
            return result.returncode
    audit = json.loads((evidence/'audit.json').read_text())
    plan = json.loads((evidence/'selection.json').read_text())
    selected = plan['selected_nodeids']
    expected = sorted(x.split('::')[1] for x in selected)
    assert len(expected) == 23 and all(x.startswith('tests/linux_browser_tests.c::') for x in selected)
    binary = ROOT/'build/linux-prototype/folderwing-linux-browser-tests'
    helper = ROOT/'build/linux-prototype/folderwing-linux-helper'
    icon = ROOT/'build/linux-prototype/folderwing.svg'
    run = ['dbus-run-session', '--']
    if args.backend == 'x11':
        run += ['xvfb-run', '-a', '--server-args=-screen 0 1400x900x24']
    run += [str(binary), str(helper), str(icon)]
    snapshot = source_snapshot()
    inputs = {
        'schema': 'folderwing-linux-browser-gate-input/v1', 'lane': 'selected+desktop-acceptance+governance-audit',
        'revision': command(['git', 'rev-parse', 'HEAD']), 'worktree_sha256': snapshot,
        'inventory_sha256': audit['inventory_digest'], 'bindings_sha256': audit['bindings_digest'],
        'selection_sha256': digest(plan), 'selected_nodeids': selected,
        'build_command': ['./linux-prototype/build.sh', '--tests'], 'run_command': run,
        'workers': 1, 'profile': {'backend': args.backend, 'renderer': 'cairo', 'fatal_criticals': True,
            'gtk': command(['pkg-config', '--modversion', 'gtk4']), 'sqlite': command(['pkg-config', '--modversion', 'sqlite3']),
            'compiler': command([os.environ.get('CC', 'cc'), '--version']).splitlines()[0],
            'kernel': platform.release(), 'machine': platform.machine(), 'cpu_count': os.cpu_count(),
            'load_average': os.getloadavg(),
            'memory_available_kib': int(re.search(r'^MemAvailable:\s+(\d+)', Path('/proc/meminfo').read_text(), re.M)[1]),
            'fixture': 'owned temporary trees/trash, native helper + link-time fsync fault helper, owned desktop associations/capture apps, distinct /tmp and /dev/shm, 10000-file desktop case',
            'network': 'none', 'windows_input': False, 'wsl_shutdown': False},
    }
    key = digest(inputs)
    (evidence/'INPUT.json').write_text(json.dumps({'input_key': key, 'inputs': inputs}, indent=2)+'\n')
    started = time.monotonic()
    with (evidence/'build.log').open('w') as log:
        built = subprocess.run(inputs['build_command'], cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
    built_seconds = time.monotonic()-started
    env = os.environ.copy()
    env.update(G_DEBUG='fatal-criticals', GTK_A11Y='none', GSK_RENDERER='cairo', GDK_BACKEND=args.backend,
               FW_LINUX_SCREENSHOT=str(evidence/'desktop.png'))
    called = time.monotonic()
    with (evidence/'acceptance.log').open('w') as log:
        result = built
        if built.returncode == 0:
            child = subprocess.Popen(run, cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
            try:
                result = subprocess.CompletedProcess(run, child.wait(timeout=90))
            except (subprocess.TimeoutExpired, KeyboardInterrupt) as error:
                # This group contains only this gate's DBus/Xvfb/test/helper
                # processes. Never stop a distro or kill by executable name.
                try:
                    os.killpg(child.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                child.wait()
                result = subprocess.CompletedProcess(run, 130 if isinstance(error, KeyboardInterrupt) else 124)
                log.write('\nGate interrupted or exceeded its 90-second bound.\n')
    text = (evidence/'acceptance.log').read_text()
    passed = sorted(re.findall(r'^PASS (test_\w+) [0-9.]+ ms$', text, re.M))
    assertions = re.search(rf'^PASS {len(expected)} selectors; (\d+) assertions; zero skips$', text, re.M)
    unchanged = source_snapshot() == snapshot
    ok = built.returncode == result.returncode == 0 and passed == expected and assertions and unchanged
    record = {'schema': 'folderwing-linux-browser-gate-receipt/v1', 'input_key': key,
              'outcome': 'passed' if ok else 'failed', 'executed_nodeids': ['tests/linux_browser_tests.c::'+x for x in passed],
              'assertions': int(assertions[1]) if assertions else 0,
              'counts': {'skips': 0, 'xfail': 0, 'deselections': 0, 'quarantine': 0, 'automatic_retries': 0, 'failures': 0 if ok else 1},
              'selector_wall_ms': {name: float(ms) for name, ms in re.findall(r'^PASS (test_\w+) ([0-9.]+) ms$', text, re.M)},
              'fixture_timing': 'Setup and teardown are included in selector wall times, not independently instrumented.',
              'source_unchanged_during_run': unchanged, 'workers': 1,
              'build_seconds': round(built_seconds, 3), 'acceptance_seconds': round(time.monotonic()-called, 3),
              'total_seconds': round(time.monotonic()-started, 3),
              'log_sha256': hashlib.sha256(text.encode()).hexdigest(),
              'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest() if binary.exists() else None,
              'measurements': [line for line in text.splitlines() if line.startswith('MEASURE ')]}
    record['receipt_id'] = digest(record)
    (evidence/'RESULT.json').write_text(json.dumps(record, indent=2)+'\n')
    print(json.dumps(record, indent=2))
    if not ok: print(text[-10000:], file=sys.stderr)
    return 0 if ok else 1

if __name__ == '__main__':
    raise SystemExit(main())
