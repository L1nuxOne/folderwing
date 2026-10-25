"""Native index performance on a read-only real Linux tree, private cache only.

Usage: python3 tests/wsl_index_bench.py /path/to/folderwing-wsl --output NEW.json
The shared protocol client consumes the first positional helper argument.
"""
import argparse
import hashlib
import json
from pathlib import Path
import statistics
import tempfile
import time

from test_wsl_index import Session


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', default='/usr')
    parser.add_argument('--min-entries', type=int, default=100000)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    if args.output.exists():
        parser.error('output must be a new evidence file')
    with tempfile.TemporaryDirectory(prefix='openpilot-native-index-budget-') as tmp:
        start = time.perf_counter()
        s = Session(args.root, Path(tmp) / 'cache', 1)
        try:
            if s.initial['state'] == 2:
                raise AssertionError('a new session cannot claim current coverage')
            while True:
                status = s.control()
                if status['state'] == 5:
                    raise AssertionError(f'native index failed: {status}')
                if status['state'] == 2:
                    break
                if time.perf_counter() - start > 30:
                    raise AssertionError('native index exceeded 30-second scan budget')
                time.sleep(.02)
            build_ms = (time.perf_counter() - start) * 1000
            if status['entries'] < args.min_entries or status['errors']:
                raise AssertionError('real tree does not meet the sealed dataset profile')
            samples = {}
            for pattern in (b'*sqlite*', b'*python3.12*', b'*zzq-no-such-native-index-name*'):
                times = []
                for _ in range(7):
                    before = time.perf_counter_ns()
                    rows, current, end = s.query(pattern)
                    times.append((time.perf_counter_ns() - before) / 1e6)
                    if current['state'] != 2 or end[0] or len(rows) > 1000:
                        raise AssertionError('query lacked complete bounded current coverage')
                if max(times) > 100:
                    raise AssertionError(f'native candidate query exceeded 100ms: {pattern!r}')
                samples[pattern.decode()] = {'samples_ms': times, 'median_ms': statistics.median(times),
                                             'p95_ms': max(times), 'candidates': len(rows)}
            process_status = Path(f'/proc/{s.p.pid}/status').read_text()
            hwm = int(next(line.split()[1] for line in process_status.splitlines() if line.startswith('VmHWM:')))
            if hwm > 96 * 1024:
                raise AssertionError(f'native index exceeded 96MiB RSS: {hwm} KiB')
            watches = sum(sum(line.startswith('inotify wd:') for line in f.read_text().splitlines())
                          for f in Path(f'/proc/{s.p.pid}/fdinfo').iterdir())
            cache = Path(tmp) / 'cache/openpilot-search-v1'
            result = {'schema': 'openpilot-native-index-budget/v1', 'root': args.root,
                      'profile': {'workers': 1, 'cache': 'warm OS cache,16MiB SQLite,mmap off',
                                  'limits': {'scan_ms': 30000, 'query_p95_ms': 100, 'rss_kib': 96 * 1024},
                                  'excludes': 'Windows/WSL launch, IPC, UI and kernel watch memory'},
                      'entries': status['entries'], 'skipped': status['skipped'], 'errors': status['errors'],
                      'scan_watch_build_ms': build_ms, 'native_peak_rss_kib': hwm,
                      'watches': watches, 'cache_files_bytes': {p.name: p.stat().st_size for p in cache.iterdir()},
                      'queries': samples, 'uid': status['uid'],
                      'helper_sha256': hashlib.sha256(Path(s.p.args[0]).read_bytes()).hexdigest()}
        finally:
            s.close()
    result['private_cache_removed'] = not Path(tmp).exists()
    with args.output.open('x') as output:
        json.dump(result, output, indent=2); output.write('\n')
    print(json.dumps(result))
    print('1 passed, 0 failed')


if __name__ == '__main__':
    main()
