"""Native Linux helper contracts. Every fixture and its trash live in a private temp directory.
Run: python3 tests/test_wsl_jobs.py /absolute/path/to/folderwing-wsl
"""
import errno
import os
from pathlib import Path
import select
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import unittest
import resource
from unittest.mock import patch

HELPER = sys.argv.pop(1) if __name__ == '__main__' and len(sys.argv) > 1 and not sys.argv[1].startswith('-') else str(Path(__file__).resolve().parents[1] / 'build/release/folderwing-wsl')
HEAD = struct.Struct('<III')
PROGRESS = struct.Struct('<QQIIIIIiII')

# Exact ownership and additive impact bindings for late destination refusals.
TEST_CONTRACTS = {
    'test_checked_read_sources_close_after_skipped_copies': {
        'primary': 'Linux integration',
        'owner': 'Checked source descriptor lifetime across skipped and refused copy conflicts',
        'sources': ['linux/fpjobs.c', 'linux/fpmount.h', 'linux/fpstage.h',
                    'linux/fpmetadata.h', 'linux/fpancestry.h', 'linux/fppath.c',
                    'linux/fpwsl.c', 'src/wslproto.h', 'tests/test_wsl_jobs.py'],
    },
    'test_unreadable_copy_preserves_conflict_order_and_target': {
        'primary': 'Linux integration',
        'owner': 'Read-open failure preserves source mount checks and conflict policy',
        'sources': ['linux/fpjobs.c', 'linux/fpmount.h', 'linux/fpstage.h',
                    'linux/fpmetadata.h', 'linux/fpancestry.h', 'linux/fppath.c',
                    'linux/fpwsl.c', 'src/wslproto.h', 'tests/test_wsl_jobs.py'],
    },
    'test_windows_bounded_copy_lengths_and_native_pseudo_file': {
        'primary': 'Linux integration',
        'owner': 'Exact Windows regular-file copy lengths without truncating native pseudo-files',
        'sources': ['linux/fpjobs.c', 'linux/fpmount.h', 'linux/fpstage.h',
                    'linux/fpmetadata.h', 'linux/fpancestry.h', 'linux/fppath.c',
                    'linux/fpwsl.c', 'src/wslproto.h', 'tests/test_wsl_jobs.py'],
    },
    'test_checked_windows_sources_reject_late_versions': {
        'primary': 'Linux integration',
        'owner': 'Checked source handles retain pathname and version guards before publication',
        'sources': ['linux/fpjobs.c', 'linux/fpmount.h', 'linux/fpstage.h',
                    'linux/fpmetadata.h', 'linux/fpancestry.h', 'linux/fppath.c',
                    'linux/fpwsl.c', 'src/wslproto.h', 'tests/test_wsl_jobs.py'],
    },
    'test_late_destination_guard_refusal_reports_each_source': {
        'primary': 'Linux integration',
        'owner': 'Native Linux destination safety and truthful job completion',
        'sources': ['linux/fpjobs.c', 'linux/fpancestry.h', 'src/wslproto.h',
                    'src/wsl.c', 'src/jobs.c', 'app/operations.c',
                    'tests/test_wsl_jobs.py'],
    },
    'test_destination_guard_successful_directory_copy_and_move': {
        'primary': 'Linux integration',
        'owner': 'Native Linux destination safety and truthful job completion',
        'sources': ['linux/fpjobs.c', 'linux/fpancestry.h', 'src/wslproto.h',
                    'src/wsl.c', 'src/jobs.c', 'app/operations.c',
                    'tests/test_wsl_jobs.py'],
    },
    'test_destination_guard_cancellation_after_rename_retains_sources': {
        'primary': 'Linux integration',
        'owner': 'Native Linux destination safety and truthful job completion',
        'sources': ['linux/fpjobs.c', 'linux/fpancestry.h', 'src/wslproto.h',
                    'src/wsl.c', 'src/jobs.c', 'app/operations.c',
                    'tests/test_wsl_jobs.py'],
    },
}

class NativeJobs(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='openpilot_native_')
        self.root = Path(self.tmp.name)
        self.src = self.root / 'source'; self.src.mkdir()
        self.dst = self.root / 'target'; self.dst.mkdir()
        self.env = dict(os.environ, XDG_DATA_HOME=str(self.root / 'data'))

    def tearDown(self):
        self.tmp.cleanup()

    def job(self, op, sources, dest='', policy=0, cancel_at=None, windows=(), host_error=0, mutate=None, directory_reply=None, source_reply=None):
        p = subprocess.Popen([HELPER, '--jobs'], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=self.env)
        def read(n):
            data = b''
            while len(data) < n:
                if not select.select([p.stdout], [], [], 10)[0]:
                    raise AssertionError('helper reply timeout')
                part = os.read(p.stdout.fileno(), n-len(data))
                if not part: raise AssertionError('helper ended before final progress')
                data += part
            return data
        def message():
            typ, n, ident = HEAD.unpack(read(12))
            self.assertLessEqual(n, 1 << 20)
            return typ, ident, read(n)
        try:
            typ, _, hello = message(); self.assertEqual(typ, 1); self.assertEqual(len(hello), 16)
            body = struct.pack('<IIII', op, policy, len(sources), 0)
            for i, path in enumerate([dest, *sources]):
                raw = os.fsencode(path)
                body += struct.pack('<II', len(raw), int(i in windows)) + raw
            p.stdin.write(HEAD.pack(10, len(body), 1) + body); p.stdin.flush()
            events = []; cancelled = False; continued = False; exported = b''; export_file = None; end_errors = []; directory_syncs = []; source_checks = []
            while True:
                typ, ident, body = message(); self.assertEqual(ident, 1)
                if mutate: mutate(typ, body)
                if typ == 59:  # WSLP_SOURCE_CHECK: Windows inventory is a host responsibility.
                    root, size = struct.unpack('<II', body[:8])
                    self.assertIn(root, windows); self.assertEqual(size, len(body)-8)
                    source_checks.append((root, body[8:]))
                    reply = source_reply(len(source_checks), root, body[8:]) if source_reply else 0
                    if isinstance(reply, tuple): code, data = reply
                    else: code, data = 60, struct.pack('<i', reply)
                    p.stdin.write(HEAD.pack(code, len(data), 1) + data); p.stdin.flush(); continue
                if typ == 54:  # WSLP_DIRECTORY_SYNC, protocol v2; host stub only.
                    root, size, metadata, pad, atime, mtime = struct.unpack('<IIIIqq', body[:32])
                    self.assertEqual(root, 0); self.assertEqual(size, len(body)-32); self.assertIn(metadata, [0, 1]); self.assertEqual(pad, 0)
                    if not metadata: self.assertEqual((atime, mtime), (0, 0))
                    directory_syncs.append(body[32:])
                    reply = directory_reply(len(directory_syncs), body[32:]) if directory_reply else 0
                    if isinstance(reply, tuple): code, data = reply
                    else: code, data = 55, struct.pack('<i', reply)
                    p.stdin.write(HEAD.pack(code, len(data), 1) + data); p.stdin.flush(); continue
                if typ == 15:
                    self.assertGreaterEqual(len(body), 32)
                    atime, mtime, replace, root, n, pad = struct.unpack('<qqIIII', body[:32])
                    self.assertEqual(root, 0); self.assertEqual(n, len(body)-32)
                    export_file = body[32:]; self.assertTrue(export_file.startswith(b'/'))
                    continue
                if typ == 16:
                    self.assertIsNotNone(export_file); self.assertLessEqual(len(body), 65536)
                    exported += body; continue
                if typ == 17:
                    self.assertEqual(len(body), 4); end_errors.append(struct.unpack('<i', body)[0])
                    p.stdin.write(HEAD.pack(18, 4, 1) + struct.pack('<i', host_error)); p.stdin.flush()
                    continue
                if typ == 12:
                    kind, error, root, n = struct.unpack('<IiII', body[:16])
                    self.assertEqual(n, len(body)-16); events.append((kind, error, root, body[16:]))
                    continue
                self.assertEqual(typ, 11)
                values = PROGRESS.unpack(body[:48])
                done, total, state, items, count, errors, skipped, error, root, n = values
                self.assertEqual(n, len(body)-48)
                if state == 1 and cancel_at == 'scanning' and not cancelled:
                    p.stdin.write(HEAD.pack(13, 0, 1)); p.stdin.flush(); cancelled = True
                if state == 2 and not continued and not cancelled:
                    # Worker must wait for acknowledgement before its first mutation.
                    code = 13 if cancel_at == 'running' else 14
                    p.stdin.write(HEAD.pack(code, 0, 1)); p.stdin.flush(); continued = True
                    cancelled = code == 13
                elif state == 2 and not cancelled and isinstance(cancel_at, int) and done >= cancel_at:
                    p.stdin.write(HEAD.pack(13, 0, 1)); p.stdin.flush(); cancelled = True
                if state >= 3:
                    p.wait(timeout=5)
                    self.assertEqual(p.returncode, 0, p.stderr.read().decode(errors='replace'))
                    return dict(state=state, errors=errors, skipped=skipped, done=done, total=total, items=items, events=events, exported=exported, export_file=export_file, end_errors=end_errors, directory_syncs=directory_syncs, source_checks=source_checks)
        finally:
            if p.poll() is None: p.kill(); p.wait()
            p.stdin.close(); p.stdout.close(); p.stderr.close()

    # Primary: engine integration. Owner: Windows-source stream inventory ACKs,
    # publication refusal and move-source retention (linux/fpjobs.c + src/wsl.c).
    def test_checked_read_sources_close_after_skipped_copies(self):
        sources = []
        for i in range(160):
            source = self.src / f'skip-{i:03d}'
            source.write_bytes(b'source stays'); sources.append(source)
            (self.dst / source.name).write_bytes(b'existing target')
        real_popen = subprocess.Popen
        def limited(*args, **kwargs):
            kwargs['preexec_fn'] = lambda: resource.setrlimit(resource.RLIMIT_NOFILE, (48, 48))
            return real_popen(*args, **kwargs)
        for policy in [1, 3]:
            with self.subTest(policy=policy), patch.object(subprocess, 'Popen', limited):
                result = self.job(0, sources, self.dst, policy=policy)
            expected = (3, 0, 160) if policy == 1 else (3, 160, 0)
            self.assertEqual((result['state'], result['errors'], result['skipped']), expected)
            if policy == 3:
                self.assertEqual(len(result['events']), 160)
                self.assertTrue(all(event[0] == 0 and event[1] == errno.EEXIST for event in result['events']))
        for source in sources:
            self.assertEqual(source.read_bytes(), b'source stays')
            self.assertEqual((self.dst / source.name).read_bytes(), b'existing target')

    def test_unreadable_copy_preserves_conflict_order_and_target(self):
        self.assertNotEqual(os.geteuid(), 0, 'Run this read-access contract as an ordinary WSL user')
        source = self.src / 'unreadable'; source.write_bytes(b'keep source'); source.chmod(0)
        target = self.dst / source.name; target.write_bytes(b'keep target')
        try:
            skipped = self.job(0, [source], self.dst, policy=1)
            self.assertEqual((skipped['errors'], skipped['skipped']), (0, 1))
            conflict = self.job(0, [source], self.dst, policy=3)
            self.assertEqual(conflict['events'][0][1], errno.EEXIST)
            denied = self.job(0, [source], self.dst, policy=2)
            self.assertEqual(denied['events'][0][1], errno.EACCES)
            self.assertEqual(target.read_bytes(), b'keep target')
        finally:
            source.chmod(0o600)
        self.assertEqual(source.read_bytes(), b'keep source')

    def test_windows_bounded_copy_lengths_and_native_pseudo_file(self):
        for size in [0, 1, 262143, 262144, 262161]:
            with self.subTest(size=size):
                source = self.src / f'length-{size}'
                data = (bytes(range(256)) * ((size + 255) // 256))[:size]
                source.write_bytes(data)
                path = subprocess.check_output(['wslpath', '-w', str(source)]).decode().strip()
                result = self.job(0, [path], self.dst, windows=(1,))
                self.assertEqual((result['state'], result['errors'], result['done']), (3, 0, size))
                self.assertEqual((self.dst / source.name).read_bytes(), data)
                self.assertEqual(source.read_bytes(), data)
        pseudo = Path('/proc/version')
        self.assertEqual(pseudo.stat().st_size, 0)
        expected = pseudo.read_bytes(); self.assertTrue(expected)
        result = self.job(0, [pseudo], self.dst)
        self.assertEqual((result['state'], result['errors']), (3, 0))
        self.assertEqual((self.dst / pseudo.name).read_bytes(), expected)

    def test_checked_windows_sources_reject_late_versions(self):
        for boundary in [1, 2]:
            for change in ['replace', 'edit', 'grow', 'truncate', 'fifo']:
                with self.subTest(boundary=boundary, change=change):
                    source = self.src / f'version-{boundary}-{change}'
                    source.write_bytes(b'original bytes')
                    target = self.dst / source.name; target.write_bytes(b'existing target')
                    def reply(n, root, suffix):
                        if n == boundary:
                            before = source.stat()
                            if change in ['replace', 'fifo']:
                                source.rename(source.with_suffix('.retained'))
                                if change == 'fifo': os.mkfifo(source)
                                else: source.write_bytes(b'new node bytes')
                            elif change == 'edit': source.write_bytes(b'edited content')
                            elif change == 'grow':
                                with source.open('ab') as stream: stream.write(b'extra')
                            else: source.write_bytes(b'')
                            if change != 'fifo': os.utime(source, ns=(before.st_atime_ns, before.st_mtime_ns))
                        return 0
                    path = subprocess.check_output(['wslpath', '-w', str(source)]).decode().strip()
                    result = self.job(0, [path], self.dst, policy=2, windows=(1,), source_reply=reply)
                    self.assertEqual(result['errors'], 1)
                    self.assertEqual(result['events'][0][1], errno.ESTALE)
                    self.assertEqual(target.read_bytes(), b'existing target')
                    self.assertTrue(source.exists())

    def test_windows_sources_require_checks_before_publish_and_remove(self):
        destination = tempfile.TemporaryDirectory(prefix='openpilot_windows_source_', dir='/dev/shm')
        self.addCleanup(destination.cleanup); self.dst = Path(destination.name)
        for op in [0, 1, 9]:
            with self.subTest(op=op):
                source = self.src/('checked-'+str(op)); source.write_bytes(b'complete bytes')
                target = self.dst/source.name; target.write_bytes(b'old target'); observations = []
                def reply(n, root, suffix):
                    observations.append((n, root, suffix, source.exists(), target.read_bytes()))
                    return 0
                windows_path = subprocess.check_output(['wslpath', '-w', str(source)]).decode().strip()
                result = self.job(op, [windows_path], self.dst, policy=2, windows=(1,), source_reply=reply)
                self.assertEqual((result['state'], result['errors']), (3, 0))
                self.assertEqual(observations, [(n, 1, b'', True, b'old target' if n<3 else b'complete bytes') for n in range(1, 4 if op==1 else 3)])
                self.assertEqual(source.exists(), op!=1); self.assertEqual(target.read_bytes(), b'complete bytes')

    def test_windows_source_refused_acks_preserve_data(self):
        destination = tempfile.TemporaryDirectory(prefix='openpilot_windows_source_', dir='/dev/shm')
        self.addCleanup(destination.cleanup); self.dst = Path(destination.name)
        for op in [0, 1, 9]:
            for boundary in range(1, 4 if op==1 else 3):
                for error in [errno.ENOTSUP, errno.EACCES, errno.EIO]:
                    with self.subTest(op=op, boundary=boundary, error=error):
                        source = self.src/'retain'; source.write_bytes(b'complete bytes')
                        target = self.dst/source.name; target.write_bytes(b'old target')
                        windows_path = subprocess.check_output(['wslpath', '-w', str(source)]).decode().strip()
                        result = self.job(op, [windows_path], self.dst, policy=2, windows=(1,), source_reply=lambda n, r, s: error if n==boundary else 0)
                        self.assertEqual((result['state'], result['errors']), (3, 1)); self.assertEqual(result['events'][0][1], error)
                        self.assertEqual(source.read_bytes(), b'complete bytes')
                        self.assertEqual(target.read_bytes(), b'complete bytes' if boundary==3 else b'old target')
                        self.assertEqual(list(self.dst.glob('.openpilot-stage-*')), [])

    def test_windows_source_bad_or_cancelled_ack_never_publishes(self):
        destination = tempfile.TemporaryDirectory(prefix='openpilot_windows_source_', dir='/dev/shm')
        self.addCleanup(destination.cleanup); self.dst = Path(destination.name)
        for reply in [(55, struct.pack('<i', 0)), (60, b'x'), (13, b''), errno.ECANCELED]:
            with self.subTest(reply=reply):
                source = self.src/'retain'; source.write_bytes(b'complete bytes'); target = self.dst/source.name; target.write_bytes(b'old target')
                windows_path = subprocess.check_output(['wslpath', '-w', str(source)]).decode().strip()
                result = self.job(1, [windows_path], self.dst, policy=2, windows=(1,), source_reply=lambda n, r, s: reply)
                self.assertEqual(result['state'], 4); self.assertEqual(source.read_bytes(), b'complete bytes'); self.assertEqual(target.read_bytes(), b'old target')
                self.assertEqual(list(self.dst.glob('.openpilot-stage-*')), [])

    def test_windows_directory_checks_refuse_before_descent(self):
        destination = tempfile.TemporaryDirectory(prefix='openpilot_windows_source_', dir='/dev/shm')
        self.addCleanup(destination.cleanup); self.dst = Path(destination.name)
        source = self.src/'tree'; source.mkdir(); (source/'child').write_bytes(b'retain')
        windows_path = subprocess.check_output(['wslpath', '-w', str(source)]).decode().strip()
        result = self.job(1, [windows_path], self.dst, windows=(1,), source_reply=lambda n, r, s: errno.ENOTSUP)
        self.assertEqual((result['state'], result['errors']), (3, 1)); self.assertEqual(result['source_checks'], [(1, b'')])
        self.assertEqual((source/'child').read_bytes(), b'retain'); self.assertEqual(list(self.dst.iterdir()), [])

    def test_stream_move_retains_source_changed_after_copy_ack_boundary(self):
        for change in ['overwrite', 'append', 'truncate', 'replace']:
            with self.subTest(change=change):
                source = self.src/'selected'; source.write_bytes(b'original bytes')
                def mutate(typ, body):
                    if typ != 17: return
                    if change == 'overwrite': source.write_bytes(b'changed source')
                    elif change == 'append':
                        with source.open('ab') as stream: stream.write(b' appended')
                    elif change == 'truncate': source.write_bytes(b'x')
                    else:
                        replacement = self.src/'replacement'; replacement.write_bytes(b'new inode'); replacement.replace(source)
                with tempfile.TemporaryDirectory(prefix='openpilot_changed_', dir='/dev/shm') as destination:
                    windows_path = subprocess.check_output(['wslpath', '-w', destination]).decode().strip()
                    result = self.job(1, [source], windows_path, windows=(0,), mutate=mutate)
                self.assertEqual(result['errors'], 1)
                self.assertEqual(result['events'][0][1], errno.ESTALE)
                self.assertTrue(source.exists())
                self.assertNotEqual(source.read_bytes(), b'original bytes')
                self.assertEqual(result['exported'], b'original bytes')
                self.assertEqual(result['end_errors'], [0])

    def test_stream_rejects_source_changed_during_read_before_host_commit(self):
        for change in ['overwrite', 'append', 'truncate']:
            with self.subTest(change=change):
                source = self.src/'selected'; source.write_bytes(b'a' * (1024 * 1024)); changed = []
                def mutate(typ, body):
                    if typ != 16 or changed: return
                    changed.append(True)
                    if change == 'overwrite': source.write_bytes(b'b' * (1024 * 1024))
                    elif change == 'append':
                        with source.open('ab') as stream: stream.write(b'new suffix')
                    else: source.write_bytes(b'truncated')
                with tempfile.TemporaryDirectory(prefix='openpilot_changed_', dir='/dev/shm') as destination:
                    windows_path = subprocess.check_output(['wslpath', '-w', destination]).decode().strip()
                    result = self.job(1, [source], windows_path, windows=(0,), mutate=mutate)
                self.assertTrue(changed); self.assertTrue(source.exists())
                self.assertEqual(result['end_errors'], [errno.ESTALE])
                self.assertEqual(result['errors'], 1)

    def test_windows_directory_sync_before_descent_and_removal(self):
        source = self.src/'tree'; source.mkdir(); (source/'child').write_bytes(b'held until ACK')
        observed = []
        def reply(n, suffix):
            observed.append((suffix, source.exists(), (source/'child').exists()))
            return 0
        with tempfile.TemporaryDirectory(prefix='openpilot_dir_protocol_', dir='/dev/shm') as dest:
            windows_path = subprocess.check_output(['wslpath', '-w', dest]).decode().strip()
            r = self.job(1, [source], windows_path, windows=(0,), directory_reply=reply)
        self.assertEqual((r['state'], r['errors']), (3, 0)); self.assertFalse(source.exists())
        self.assertEqual(observed, [(b'/tree', True, True), (b'/tree', True, False)])
        # The host is a protocol stub: committed file bytes are retained here,
        # not claimed to be written or flushed by Windows in this Linux test.
        self.assertEqual(r['exported'], b'held until ACK')

    def test_windows_initial_directory_error_prevents_child_removal(self):
        for error in [errno.EIO, errno.ENOTSUP, errno.EACCES]:
            with self.subTest(error=error):
                source = self.src/('tree-'+str(error)); source.mkdir(); (source/'child').write_bytes(b'retain')
                with tempfile.TemporaryDirectory(prefix='openpilot_dir_protocol_', dir='/dev/shm') as dest:
                    windows_path = subprocess.check_output(['wslpath', '-w', dest]).decode().strip()
                    r = self.job(1, [source], windows_path, windows=(0,), directory_reply=lambda n, p: error)
                    self.assertEqual(list((Path(dest)/source.name).iterdir()), [])
                self.assertEqual((r['state'], r['errors']), (3, 1)); self.assertEqual(r['events'][0][1], error)
                self.assertEqual((source/'child').read_bytes(), b'retain'); self.assertEqual(r['exported'], b'')

    def test_windows_final_directory_error_retains_source_directory(self):
        for child in [False, True]:
            with self.subTest(child=child):
                source = self.src/('tree-'+str(child)); source.mkdir()
                if child: (source/'child').write_bytes(b'acknowledged by host stub')
                with tempfile.TemporaryDirectory(prefix='openpilot_dir_protocol_', dir='/dev/shm') as dest:
                    windows_path = subprocess.check_output(['wslpath', '-w', dest]).decode().strip()
                    r = self.job(1, [source], windows_path, windows=(0,), directory_reply=lambda n, p: errno.EIO if n==2 else 0)
                self.assertEqual((r['state'], r['errors']), (3, 1)); self.assertTrue(source.is_dir()); self.assertEqual(list(source.iterdir()), [])
                self.assertEqual(len(r['directory_syncs']), 2)

    def test_windows_directory_cancel_ack_retains_remaining_source(self):
        for boundary in [1, 2]:
            with self.subTest(boundary=boundary):
                source = self.src/('tree-'+str(boundary)); source.mkdir()
                with tempfile.TemporaryDirectory(prefix='openpilot_dir_protocol_', dir='/dev/shm') as dest:
                    windows_path = subprocess.check_output(['wslpath', '-w', dest]).decode().strip()
                    r = self.job(1, [source], windows_path, windows=(0,), directory_reply=lambda n, p: errno.ECANCELED if n==boundary else 0)
                self.assertEqual((r['state'], r['errors']), (4, 0)); self.assertTrue(source.is_dir())

    def test_windows_malformed_directory_ack_never_removes_source(self):
        for reply in [(55, b''), (11, struct.pack('<i', 0)), (55, b'oversized')]:
            with self.subTest(reply=reply):
                source = self.src/('tree-'+str(len(list(self.src.iterdir())))); source.mkdir(); (source/'child').write_bytes(b'retain')
                with tempfile.TemporaryDirectory(prefix='openpilot_dir_protocol_', dir='/dev/shm') as dest:
                    windows_path = subprocess.check_output(['wslpath', '-w', dest]).decode().strip()
                    r = self.job(1, [source], windows_path, windows=(0,), directory_reply=lambda n, p: reply)
                self.assertEqual(r['state'], 4); self.assertEqual((source/'child').read_bytes(), b'retain'); self.assertEqual(r['exported'], b'')

    def test_windows_final_directory_footer_preserves_fractional_source_time(self):
        source = self.src/'precise'; source.mkdir(); ns = 1697440326627072100; os.utime(source, ns=(ns, ns)); footers = []
        def observe(kind, body):
            if kind == 54: footers.append(struct.unpack('<IIIIqq', body[:32]))
        with tempfile.TemporaryDirectory(prefix='openpilot_dir_protocol_', dir='/dev/shm') as dest:
            windows_path = subprocess.check_output(['wslpath', '-w', dest]).decode().strip()
            r = self.job(0, [source], windows_path, windows=(0,), mutate=observe)
        self.assertEqual(r['errors'], 0); self.assertEqual([f[2] for f in footers], [0, 1]); self.assertEqual(footers[1][-1], ns)

    def test_windows_symlink_move_requires_parent_directory_ack(self):
        for error in [0, errno.EIO]:
            with self.subTest(error=error):
                source = self.src/('link-'+str(error)); source.symlink_to('literal-target')
                with tempfile.TemporaryDirectory(prefix='openpilot_dir_protocol_', dir='/dev/shm') as dest:
                    windows_path = subprocess.check_output(['wslpath', '-w', dest]).decode().strip()
                    r = self.job(1, [source], windows_path, windows=(0,), directory_reply=lambda n, p: error)
                    self.assertEqual(os.readlink(Path(dest)/source.name), 'literal-target')
                self.assertEqual(r['errors'], int(error != 0)); self.assertEqual(source.is_symlink(), error != 0); self.assertEqual(r['directory_syncs'], [b''])

    def test_metadata_and_symlinks(self):
        tree = self.src / 'tree'; tree.mkdir(); os.chmod(tree, 0o750)
        a = tree / 'executable'; a.write_bytes(b'payload'); os.chmod(a, 0o754)
        os.utime(a, ns=(1700000000123456789, 1700000000987654321))
        (tree / 'broken').symlink_to('missing'); (tree / 'directory_link').symlink_to('../../target', target_is_directory=True)
        mode = 0o754
        if os.geteuid() == 0:
            os.chown(a, 1000, 1000); mode = 0o4754; os.chmod(a, mode)
        before = a.stat(); result = self.job(0, [tree], self.dst)
        self.assertEqual(result['errors'], 0)
        self.assertEqual((self.dst/'tree/executable').stat().st_mode & 0o7777, mode)
        self.assertEqual((self.dst/'tree/executable').stat().st_uid, before.st_uid)
        if os.geteuid() == 0: self.assertEqual((self.dst/'tree/executable').stat().st_gid, before.st_gid)
        self.assertEqual((self.dst/'tree/executable').stat().st_mtime_ns, before.st_mtime_ns)
        self.assertEqual((self.dst/'tree').stat().st_mode & 0o7777, 0o750)
        self.assertEqual(os.readlink(self.dst/'tree/broken'), 'missing')
        self.assertEqual(os.readlink(self.dst/'tree/directory_link'), '../../target')

    def test_conflicts_and_case(self):
        a = self.src/'Foo'; a.write_text('new'); (self.dst/'Foo').write_text('old')
        result = self.job(0, [a], str(self.dst)+'/', 1)
        self.assertEqual(result['skipped'], 1); self.assertEqual((self.dst/'Foo').read_text(), 'old')
        self.assertEqual(result['events'][0][2:], (0, b'/Foo'))
        self.assertEqual(self.job(0, [a], self.dst, 0)['errors'], 0)
        self.assertEqual((self.dst/'Foo (2)').read_text(), 'new')
        self.assertEqual(self.job(0, [a], self.dst, 2)['errors'], 0)
        self.assertEqual((self.dst/'Foo').read_text(), 'new')
        (self.src/'foo').write_text('lower')
        self.job(0, [self.src/'foo'], self.dst)
        self.assertEqual((self.dst/'foo').read_text(), 'lower')

    def test_directory_merge_move_retains_skipped_source(self):
        a = self.src/'tree'; a.mkdir(); (a/'same').write_text('new'); (a/'extra').write_text('extra')
        b = self.dst/'tree'; b.mkdir(); (b/'same').write_text('old')
        result = self.job(1, [a], self.dst, 1)
        self.assertEqual(result['skipped'], 1); self.assertEqual(result['errors'], 0)
        self.assertTrue((a/'same').exists()); self.assertFalse((a/'extra').exists())
        self.assertEqual((b/'same').read_text(), 'old'); self.assertEqual((b/'extra').read_text(), 'extra')

    def test_delete_unlinks_symlink_without_traversal(self):
        outside = self.dst/'keep'; outside.write_text('keep')
        a = self.src/'tree'; a.mkdir(); (a/'link').symlink_to(self.dst, target_is_directory=True)
        result = self.job(2, [a]); self.assertEqual(result['errors'], 0)
        self.assertFalse(a.exists()); self.assertEqual(outside.read_text(), 'keep')

    def test_cancel_before_first_mutation(self):
        a = self.src/'a'; a.write_text('new'); b = self.dst/'a'; b.write_text('old')
        result = self.job(0, [a], self.dst, 2, cancel_at='running')
        self.assertEqual(result['state'], 4); self.assertEqual(b.read_text(), 'old')
        self.assertEqual(list(self.dst.iterdir()), [b])

    def test_cancel_in_flight_preserves_overwrite_destination(self):
        a = self.src/'big'; b = self.dst/'big'; b.write_text('original')
        with a.open('wb') as f: f.truncate(512 * 1024 * 1024)
        result = self.job(0, [a], self.dst, 2, cancel_at=1)
        self.assertEqual(result['state'], 4); self.assertEqual(b.read_text(), 'original')
        self.assertEqual(list(self.dst.iterdir()), [b])

    def test_into_self_and_symlink_alias_are_refused(self):
        a = self.src/'tree'; a.mkdir(); b = a/'sub'; b.mkdir()
        self.assertEqual(self.job(0, [a], b)['errors'], 1)
        alias = self.dst/'alias'; alias.symlink_to(b, target_is_directory=True)
        self.assertEqual(self.job(0, [a], alias)['errors'], 1)
        self.assertEqual(list(b.iterdir()), [])

    def destination_guard_case(self, label, count):
        root = self.root/label; root.mkdir()
        destination = root/'destination'; destination.mkdir()
        sources = []
        for index in range(count):
            source = root/('selected-'+str(index)); source.mkdir()
            (source/'payload').write_bytes(b'selected source bytes')
            (source/'inside').mkdir()
            prior = destination/source.name; prior.mkdir()
            (prior/'payload').write_bytes(b'previous destination bytes')
            sources.append(source)
        (destination/'unselected').write_bytes(b'retain destination sibling')
        return root, destination, sources

    def assert_destination_guard_retention(self, destination, sources):
        for source in sources:
            self.assertEqual((source/'payload').read_bytes(), b'selected source bytes')
            self.assertEqual(list((source/'inside').iterdir()), [])
            self.assertEqual(set(p.name for p in source.iterdir()), {'payload', 'inside'})
            self.assertEqual((destination/source.name/'payload').read_bytes(), b'previous destination bytes')
        self.assertEqual((destination/'unselected').read_bytes(), b'retain destination sibling')
        self.assertEqual(set(p.name for p in destination.iterdir()), {'unselected', *(p.name for p in sources)})

    def test_late_destination_guard_refusal_reports_each_source(self):
        for op in [0, 1]:
            for change, count in [('missing', 1), ('missing', 2), ('descendant-alias', 1)]:
                with self.subTest(op=op, change=change, count=count):
                    root, destination, sources = self.destination_guard_case(f'refuse-{op}-{change}-{count}', count)
                    retained = root/'retained-destination'; changed = []
                    def mutate(typ, body):
                        if typ != 11 or PROGRESS.unpack(body[:48])[2] != 2 or changed: return
                        destination.rename(retained)
                        if change == 'descendant-alias': destination.symlink_to(sources[0]/'inside', target_is_directory=True)
                        changed.append(True)
                    result = self.job(op, sources, destination, policy=2, mutate=mutate)
                    self.assertEqual(changed, [True])
                    self.assert_destination_guard_retention(retained, sources)
                    if change == 'missing': self.assertFalse(os.path.lexists(destination))
                    else: self.assertEqual(os.readlink(destination), str(sources[0]/'inside'))
                    # DONE with errors is the established partial-job contract:
                    # the host must receive failures, not a clean Completed result.
                    self.assertEqual((result['state'], result['errors'], result['skipped']), (3, count, 0))
                    self.assertEqual(result['events'], [(0, errno.EINVAL, i, b'') for i in range(1, count+1)])
                    self.assertEqual(result['items'], count)
                    self.assertEqual(result['done'], 0)

    def test_destination_guard_successful_directory_copy_and_move(self):
        for op in [0, 1]:
            with self.subTest(op=op):
                _, destination, sources = self.destination_guard_case('success-'+str(op), 1)
                result = self.job(op, sources, destination, policy=2)
                self.assertEqual((result['state'], result['errors'], result['skipped']), (3, 0, 0))
                self.assertEqual(result['events'], [])
                self.assertEqual((destination/sources[0].name/'payload').read_bytes(), b'selected source bytes')
                self.assertEqual((destination/'unselected').read_bytes(), b'retain destination sibling')
                if op == 0: self.assertEqual((sources[0]/'payload').read_bytes(), b'selected source bytes')
                else: self.assertFalse(sources[0].exists())

    def test_destination_guard_cancellation_after_rename_retains_sources(self):
        for op in [0, 1]:
            with self.subTest(op=op):
                root, destination, sources = self.destination_guard_case('cancel-'+str(op), 2)
                retained = root/'retained-destination'; changed = []
                def mutate(typ, body):
                    if typ == 11 and PROGRESS.unpack(body[:48])[2] == 2 and not changed:
                        destination.rename(retained); changed.append(True)
                result = self.job(op, sources, destination, policy=2, mutate=mutate, cancel_at='running')
                self.assertEqual(changed, [True]); self.assertFalse(os.path.lexists(destination))
                self.assert_destination_guard_retention(retained, sources)
                self.assertEqual((result['state'], result['errors'], result['skipped'], result['items']), (4, 0, 0, 0))
                self.assertEqual(result['events'], []); self.assertEqual(result['done'], 0)

    def test_destination_symlink_is_never_followed(self):
        a = self.src/'a'; a.write_text('new'); keep = self.dst/'keep'; keep.write_text('safe')
        (self.dst/'a').symlink_to(keep)
        result = self.job(0, [a], self.dst, 2)
        self.assertEqual(result['errors'], 1); self.assertEqual(keep.read_text(), 'safe')
        self.assertTrue((self.dst/'a').is_symlink())

    def test_raw_names_and_exact_rename_create(self):
        raw = os.fsencode(self.src) + b'/colon:quote\'\ninvalid\xff'
        with open(raw, 'wb') as f: f.write(b'raw')
        result = self.job(0, [raw], self.dst); self.assertEqual(result['errors'], 0)
        self.assertEqual(os.listdir(os.fsencode(self.dst)), [raw.rsplit(b'/',1)[1]])
        new = self.src/'new'; self.assertEqual(self.job(6, [new])['errors'], 0)
        renamed = self.src/'NEW'; self.assertEqual(self.job(4, [new], renamed)['errors'], 0)
        self.assertFalse(new.exists()); self.assertTrue(renamed.exists())
        self.assertEqual(self.job(5, [self.src/'folder'])['errors'], 0)
        self.assertEqual(self.job(6, [renamed])['errors'], 1)

    def test_template_creates_writable_copy(self):
        template = self.src/'template'; template.write_text('template'); os.chmod(template, 0o444)
        result = self.job(6, [self.dst/'new'], template)
        self.assertEqual(result['errors'], 0); self.assertEqual((self.dst/'new').read_text(), 'template')
        self.assertTrue((self.dst/'new').stat().st_mode & 0o200)

    def test_special_node_copy_failure_keeps_source(self):
        a = self.src/'tree'; a.mkdir(); os.mkfifo(a/'pipe')
        b = self.dst/'tree'; b.mkdir()  # force merge rather than atomic rename
        result = self.job(0, [a], self.dst, 2)
        self.assertEqual(result['errors'], 1); self.assertTrue((a/'pipe').exists())
        self.assertEqual(result['events'][0][1], errno.ENOTSUP)

    def test_trash_has_recovery_metadata(self):
        a = self.src/"trash ' me"; a.write_text('recoverable')
        result = self.job(3, [a]); self.assertEqual(result['errors'], 0); self.assertFalse(a.exists())
        trash = self.root/'data/Trash'
        files = list((trash/'files').iterdir()); info = list((trash/'info').iterdir())
        self.assertEqual(len(files), 1); self.assertEqual(files[0].read_text(), 'recoverable')
        text = info[0].read_text(); self.assertIn("trash%20%27%20me", text); self.assertIn('DeletionDate=', text)
        self.assertEqual(trash.stat().st_mode & 0o777, 0o700)

    def test_trash_failure_retains_source(self):
        a = self.src/'retain'; a.write_text('keep')
        # The wire contract identifies mounted Windows sources explicitly;
        # Linux recycle must refuse them rather than permanently deleting them.
        windows_path = subprocess.check_output(['wslpath', '-w', str(a)]).decode().strip()
        result = self.job(3, [windows_path], windows=(1,))
        self.assertEqual(result['errors'], 1); self.assertEqual(a.read_text(), 'keep')
        self.assertEqual(result['events'][0][1], errno.ENOTSUP)

    def test_cross_volume_directory_move(self):
        # /dev/shm is a distinct tmpfs on the required Linux acceptance profile.
        with tempfile.TemporaryDirectory(prefix='openpilot_move_', dir='/dev/shm') as destination:
            self.assertNotEqual(os.stat(destination).st_dev, self.root.stat().st_dev)
            tree = self.src/'move'; tree.mkdir(); (tree/'run').write_text('move'); os.chmod(tree/'run', 0o754)
            (tree/'link').symlink_to('run')
            result = self.job(1, [tree], destination)
            self.assertEqual(result['errors'], 0); self.assertFalse(tree.exists())
            self.assertEqual((Path(destination)/'move/run').read_text(), 'move')
            self.assertEqual((Path(destination)/'move/run').stat().st_mode & 0o777, 0o754)
            self.assertEqual(os.readlink(Path(destination)/'move/link'), 'run')

    def test_export_stream_frames(self):
        a = self.src/'payload'; a.write_bytes(b'exported' * 20000)
        windows_path = subprocess.check_output(['wslpath', '-w', str(self.dst)]).decode().strip()
        result = self.job(0, [a], windows_path, windows=(0,))
        self.assertEqual(result['errors'], 0); self.assertEqual(result['exported'], a.read_bytes())
        self.assertEqual(result['export_file'], b'/payload'); self.assertEqual(result['done'], a.stat().st_size)

    def test_export_stream_failure_retains_move_source(self):
        a = self.src/'payload'; a.write_text('retain')
        with tempfile.TemporaryDirectory(prefix='openpilot_export_', dir='/dev/shm') as destination:
            windows_path = subprocess.check_output(['wslpath', '-w', destination]).decode().strip()
            result = self.job(1, [a], windows_path, windows=(0,), host_error=errno.EACCES)
            self.assertEqual(result['errors'], 1); self.assertEqual(result['done'], 0)
            self.assertEqual(a.read_text(), 'retain'); self.assertEqual(result['events'][0][1], errno.EACCES)

    def test_windows_drive_root_is_not_a_source(self):
        result = self.job(0, ['C:\\'], self.dst, windows=(1,), cancel_at='scanning')
        self.assertEqual(result['state'], 5); self.assertEqual(result['errors'], 1)
        self.assertEqual(list(self.dst.iterdir()), [])

    def test_missing_source_reports_once(self):
        result = self.job(0, [self.src/'missing'], self.dst)
        self.assertEqual(result['errors'], 1); self.assertEqual(len(result['events']), 1)
        self.assertEqual(result['events'][0][2], 1)

    def test_symlink_file_folder_and_literal_targets(self):
        file = self.src/'Case β:star*$(literal);'; file.write_text('keep')
        folder = self.src/'folder'; folder.mkdir(); (folder/'child').write_text('nested')
        for source in [file, folder]:
            for op in [7, 8]:
                output = self.dst/f'{source.name}-{op}'
                result = self.job(op, [source], output, policy=3)
                self.assertEqual(result['errors'], 0)
                expected = os.path.relpath(source, self.dst) if op == 7 else str(source)
                self.assertEqual(os.readlink(output), expected)
                self.assertEqual(output.resolve(), source.resolve())
        self.assertEqual(file.read_text(), 'keep')
        self.assertEqual((folder/'child').read_text(), 'nested')
        raw = os.fsencode(self.src)+b'/raw\xffback\\slash'
        fd = os.open(raw, os.O_CREAT|os.O_WRONLY, 0o600); os.close(fd)
        output = self.dst/'raw-link'
        self.assertEqual(self.job(7, [raw], output, policy=3)['errors'], 0)
        self.assertEqual(os.readlink(os.fsencode(output)), b'../source/raw\xffback\\slash')

    def test_symlink_physical_parents_and_moving_a_tree(self):
        real = self.root/'real'; (real/'source').mkdir(parents=True); (real/'destination').mkdir()
        file = real/'source/file'; file.write_text('relocatable')
        source_alias = self.root/'source_alias'; source_alias.symlink_to(real/'source', target_is_directory=True)
        dest_alias = self.root/'dest_alias'; dest_alias.symlink_to(real/'destination', target_is_directory=True)
        output = dest_alias/'link'
        self.assertEqual(self.job(7, [source_alias/'file'], output, policy=3)['errors'], 0)
        self.assertEqual(os.readlink(output), '../source/file')
        moved = self.root/'moved'; real.rename(moved)
        self.assertEqual((moved/'destination/link').read_text(), 'relocatable')
        # Linking to an ancestor directory must use a minimal relative path.
        child = moved/'source/child'; child.mkdir()
        self.assertEqual(self.job(7, [moved/'source'], child/'parent', policy=3)['errors'], 0)
        self.assertEqual(os.readlink(child/'parent'), '..')
        self.assertEqual(self.job(7, [moved/'source'], moved/'source/self', policy=3)['errors'], 0)
        self.assertEqual(os.readlink(moved/'source/self'), '.')
        # Prefixes are case-sensitive and matched by full component.
        upper = self.root/'Foo'; upper.mkdir(); lower = self.root/'foo'; lower.mkdir()
        (upper/'payload').write_text('case')
        self.assertEqual(self.job(7, [upper/'payload'], lower/'link', policy=3)['errors'], 0)
        self.assertEqual(os.readlink(lower/'link'), '../Foo/payload')

    def test_symlink_collisions_never_replace_any_existing_node(self):
        source = self.src/'source'; source.write_text('source')
        file = self.dst/'file'; file.write_text('original')
        folder = self.dst/'directory'; folder.mkdir(); (folder/'keep').write_text('keep')
        broken = self.dst/'broken'; broken.symlink_to('missing')
        live = self.dst/'live'; live.symlink_to(file)
        for output in [file, folder, broken, live]:
            result = self.job(8, [source], output, policy=3)
            self.assertEqual(result['errors'], 1)
            self.assertEqual(result['events'][0][1], errno.EEXIST)
        self.assertEqual(file.read_text(), 'original'); self.assertEqual((folder/'keep').read_text(), 'keep')
        self.assertEqual(os.readlink(broken), 'missing'); self.assertEqual(os.readlink(live), str(file))
        self.assertEqual(source.read_text(), 'source')

    def test_symlink_cancel_missing_special_and_leaf_symlinks(self):
        source = self.src/'source'; source.write_text('keep'); output = self.dst/'link'
        result = self.job(7, [source], output, policy=3, cancel_at='running')
        self.assertEqual(result['state'], 4); self.assertFalse(output.is_symlink())
        self.assertEqual(source.read_text(), 'keep')
        result = self.job(7, [self.src/'missing'], output, policy=3)
        self.assertEqual(result['events'][0][1], errno.ENOENT); self.assertFalse(output.is_symlink())
        fifo = self.src/'fifo'; os.mkfifo(fifo)
        result = self.job(8, [fifo], output, policy=3)
        self.assertEqual(result['events'][0][1], errno.EOPNOTSUPP)
        broken = self.src/'broken'; broken.symlink_to('missing')
        self.assertEqual(self.job(8, [broken], output, policy=3)['errors'], 0)
        self.assertEqual(os.readlink(output), str(broken))

    def test_symlink_windows_target_is_resolved_without_shell_evaluation(self):
        # An actual Windows drive fixture, not a Linux UNC path tagged Windows.
        with tempfile.TemporaryDirectory(prefix='openpilot_link_', dir=Path(HELPER).parent) as scratch:
            source = Path(scratch)/'Case β $(literal)%.dat'; source.write_text('windows')
            windows = subprocess.check_output(['wslpath', '-w', str(source)]).decode().rstrip('\n')
            self.assertRegex(windows, r'^[A-Za-z]:\\')
            for op in [7, 8]:
                output = self.dst/f'windows-{op}'
                result = self.job(op, [windows], output, policy=3, windows=(1,))
                self.assertEqual(result['errors'], 0)
                self.assertEqual(output.read_text(), 'windows')
                self.assertEqual(os.readlink(output), os.path.relpath(source, self.dst) if op==7 else str(source))

    def test_symlink_execute_only_parents_and_permission_denial(self):
        self.assertNotEqual(os.geteuid(), 0, 'Run native jobs contracts as the default unprivileged WSL user')
        source = self.src/'source'; source.write_text('keep'); output = self.dst/'link'
        self.src.chmod(0o111); self.dst.chmod(0o300)
        try:
            self.assertEqual(self.job(7, [source], output, policy=3)['errors'], 0)
            self.assertEqual(output.read_text(), 'keep')
        finally:
            self.src.chmod(0o700); self.dst.chmod(0o700)
        self.dst.chmod(0o500)
        try:
            result = self.job(8, [source], self.dst/'denied', policy=3)
            self.assertEqual(result['events'][0][1], errno.EACCES)
            self.assertFalse((self.dst/'denied').is_symlink())
        finally:
            self.dst.chmod(0o700)

    def test_shell_reports_hex_cwd_and_keeps_user_prompt(self):
        home = self.root/'home'; home.mkdir(); (home/'.bashrc').write_text("PS1='user-prompt> '\nPROMPT_COMMAND='true'\n")
        folder = self.src/"it's:here"; folder.mkdir()
        env = dict(self.env, HOME=str(home), WSL_DISTRO_NAME='Ubuntu')
        arg = 'x' + os.fsencode(folder).hex()
        result = subprocess.run([HELPER, '--shell', arg], input=b'pwd\nexit\n', stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn(b'9;9;wsl:Ubuntu;' + os.fsencode(folder).hex().encode(), result.stdout)
        self.assertIn(b'user-prompt>', result.stdout)
        self.assertIn(b'133;B', result.stdout)

if __name__ == '__main__':
    unittest.main(verbosity=2)
