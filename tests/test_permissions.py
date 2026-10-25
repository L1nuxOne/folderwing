"""One-item Linux permissions contract; isolated temporary fixtures only."""
import errno
import os
from pathlib import Path
import struct
import select
import time
import subprocess
import sys
import tempfile
import unittest

HELPER = sys.argv.pop(1)
HEAD = struct.Struct('<III')
STATE = struct.Struct('<QQqIIII')

class Permissions(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='openpilot_permissions_')
        self.root = Path(self.tmp.name)
        self.item = self.root / 'file β:star*$(literal);'
        self.item.write_bytes(b'unchanged content')
        self.item.chmod(0o640)

    def tearDown(self):
        self.tmp.cleanup()

    def call(self, path=None, mode=0, expected=None, uid=None):
        raw = os.fsencode(path or self.item)
        request = struct.pack('<IIII', int(expected is not None), mode, len(raw), 0)
        request += expected or bytes(STATE.size)
        demote = (lambda: os.setuid(uid)) if uid is not None else None
        run = subprocess.run([HELPER, '--permissions'], input=HEAD.pack(21, len(request)+len(raw), 7)+request+raw,
                             capture_output=True, timeout=10, preexec_fn=demote)
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(HEAD.unpack(run.stdout[:12]), (1, 16, 0))
        self.assertEqual(HEAD.unpack(run.stdout[28:40]), (22, 304, 7))
        self.assertEqual(len(run.stdout), 344)
        error, changed = struct.unpack('<iI', run.stdout[40:48])
        metadata = run.stdout[48:88]
        return error, changed, metadata, run.stdout[88:216].split(b'\0')[0], run.stdout[216:].split(b'\0')[0]

    def test_query_is_fresh_and_read_only(self):
        before = self.item.stat()
        err, changed, meta, owner, group = self.call()
        values = STATE.unpack(meta)
        self.assertEqual((err, changed), (0, 0))
        self.assertEqual(values[:2], (before.st_dev, before.st_ino))
        self.assertEqual(values[4] & 0o7777, 0o640)
        self.assertEqual(values[5:], (before.st_uid, before.st_gid))
        self.assertEqual(self.item.stat().st_ctime_ns, before.st_ctime_ns)
        self.assertTrue(owner); self.assertTrue(group)
        self.item.chmod(0o751)
        self.assertEqual(STATE.unpack(self.call()[2])[4] & 0o7777, 0o751)

    def test_apply_literal_filename_and_verify_actual_mode(self):
        expected = self.call()[2]
        err, changed, meta, _, _ = self.call(mode=0o4751, expected=expected)
        self.assertEqual((err, changed), (0, 1))
        self.assertEqual(STATE.unpack(meta)[4] & 0o7777, self.item.stat().st_mode & 0o7777)
        self.assertEqual(self.item.stat().st_mode & 0o7777, 0o4751)
        self.assertEqual(self.item.read_bytes(), b'unchanged content')

    def test_repair_an_unreadable_file(self):
        self.item.chmod(0)
        expected = self.call()[2]
        self.assertEqual(self.call(mode=0o600, expected=expected)[:2], (0, 1))
        self.assertEqual(self.item.stat().st_mode & 0o7777, 0o600)

    def test_directory_change_is_nonrecursive(self):
        folder = self.root / 'directory'; folder.mkdir()
        nested = folder / 'nested'; nested.write_text('keep'); nested.chmod(0o640)
        expected = self.call(folder)[2]
        self.assertEqual(self.call(folder, 0o1750, expected)[:2], (0, 1))
        self.assertEqual(folder.stat().st_mode & 0o7777, 0o1750)
        self.assertEqual(nested.stat().st_mode & 0o7777, 0o640)

    def test_symlinks_and_special_files_are_refused(self):
        link = self.root / 'link'; link.symlink_to(self.item)
        self.assertEqual(self.call(link)[0], errno.EOPNOTSUPP)
        folder = self.root / 'directory'; folder.mkdir()
        dlink = self.root / 'dlink'; dlink.symlink_to(folder)
        self.assertEqual(self.call(str(dlink)+'/')[0], errno.EOPNOTSUPP)
        fifo = self.root / 'fifo'; os.mkfifo(fifo)
        self.assertEqual(self.call(fifo)[0], errno.EOPNOTSUPP)
        self.assertEqual(self.item.stat().st_mode & 0o7777, 0o640)

    def test_changed_mode_or_replaced_item_is_not_modified(self):
        expected = self.call()[2]
        self.item.chmod(0o600)
        self.assertEqual(self.call(mode=0o777, expected=expected)[:2], (errno.ESTALE, 0))
        self.assertEqual(self.item.stat().st_mode & 0o7777, 0o600)
        expected = self.call()[2]
        self.item.rename(self.root / 'original')
        self.item.write_text('replacement'); self.item.chmod(0o640)
        self.assertEqual(self.call(mode=0o777, expected=expected)[:2], (errno.ESTALE, 0))
        self.assertEqual(self.item.stat().st_mode & 0o7777, 0o640)

    def test_permission_denied_preserves_the_mode(self):
        self.assertEqual(os.geteuid(), 0, 'Run this isolated contract gate with WSL --user root')
        self.root.chmod(0o755)
        expected = self.call(uid=65534)[2]
        result = self.call(mode=0o777, expected=expected, uid=65534)
        self.assertEqual(result[:2], (errno.EPERM, 0))
        self.assertEqual(self.item.stat().st_mode & 0o7777, 0o640)

    def test_invalid_mode_missing_path_and_malformed_packets(self):
        expected = self.call()[2]
        self.assertEqual(self.call(mode=0o10000, expected=expected)[:2], (errno.EINVAL, 0))
        self.assertEqual(self.call(self.root/'missing')[0], errno.ENOENT)
        for raw in [b'', b'/tmp\0injected']:
            request = struct.pack('<IIII', 0, 0, len(raw), 0)+bytes(40)+raw
            run = subprocess.run([HELPER, '--permissions'], input=HEAD.pack(21, len(request), 1)+request,
                                 capture_output=True, timeout=10)
            self.assertEqual(run.returncode, 1)
            self.assertEqual(len(run.stdout), 28)  # HELLO only, no accepted mutation.
        self.assertEqual(self.item.stat().st_mode & 0o7777, 0o640)

    def test_session_streams_fresh_guarded_requests_before_eof(self):
        # Every response must arrive while stdin stays open. This exercises the
        # same framing as the bulk worker, including an item error between
        # successful requests and mixed chmod/identity/chown operations.
        with subprocess.Popen([HELPER, '--permissions'], stdin=subprocess.PIPE,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE) as proc:
            def read_exact(size):
                end = time.monotonic() + 5
                data = b''
                while len(data) < size:
                    left = end - time.monotonic()
                    self.assertGreater(left, 0, 'session response deadline')
                    self.assertTrue(select.select([proc.stdout], [], [], left)[0],
                                    'helper must answer without stdin EOF')
                    part = os.read(proc.stdout.fileno(), size-len(data))
                    self.assertTrue(part, 'unexpected session EOF')
                    data += part
                return data
            request_id = 0
            def exchange(kind, body, answer, size):
                nonlocal request_id
                request_id += 1
                proc.stdin.write(HEAD.pack(kind, len(body), request_id)+body)
                proc.stdin.flush()
                self.assertEqual(HEAD.unpack(read_exact(12)), (answer, size, request_id))
                return read_exact(size)
            def permission(path, expected=None, mode=0):
                raw = os.fsencode(path)
                body = struct.pack('<IIII', int(expected is not None), mode, len(raw), 0)
                return exchange(21, body+(expected or bytes(40))+raw, 22, 304)
            try:
                self.assertEqual(HEAD.unpack(read_exact(12)), (1, 16, 0))
                hello = struct.unpack('<IIII', read_exact(16))
                self.assertEqual(hello[2:], (os.getuid(), proc.pid))
                first = permission(self.item)
                self.assertEqual(struct.unpack('<iI', first[:8]), (0, 0))
                self.item.chmod(0o600)
                stale = permission(self.item, first[8:48], 0o777)
                self.assertEqual(struct.unpack('<iI', stale[:8]), (errno.ESTALE, 0))
                fresh = permission(self.item)
                applied = permission(self.item, fresh[8:48], 0o751)
                self.assertEqual(struct.unpack('<iI', applied[:8]), (0, 1))
                self.assertEqual(self.item.stat().st_mode & 0o7777, 0o751)
                missing = permission(self.root/'missing')
                self.assertEqual(struct.unpack('<iI', missing[:8]), (errno.ENOENT, 0))
                identity = exchange(23, struct.pack('<II128s128s', 1, 0,
                                    str(os.getuid()).encode(), b''), 24, 280)
                self.assertEqual(struct.unpack('<iIIIII', identity[:24]),
                                 (0, 0, 1, os.getuid(), 0, 0))
                current = permission(self.item)
                before = self.item.stat()
                raw = os.fsencode(self.item)
                ownership = exchange(25, struct.pack('<IIII', 1, os.getuid(), 0, len(raw))
                                     +current[8:48]+raw, 26, 304)
                self.assertEqual(struct.unpack('<iI', ownership[:8]), (0, 0))
                self.assertEqual(self.item.stat().st_ctime_ns, before.st_ctime_ns)
                self.assertEqual(self.item.read_bytes(), b'unchanged content')
                proc.stdin.close()
                self.assertEqual(proc.wait(timeout=5), 0)
                self.assertEqual(proc.stdout.read(), b'')  # one HELLO per session
            finally:
                if proc.poll() is None:
                    proc.kill(); proc.wait(timeout=5)

    def test_session_rejects_partial_and_invalid_followup_frames(self):
        raw = os.fsencode(self.item)
        query = struct.pack('<IIII', 0, 0, len(raw), 0)+bytes(40)+raw
        first = HEAD.pack(21, len(query), 1)+query
        for tail in [b'x', HEAD.pack(21, 0, 2), HEAD.pack(999, 0, 2)]:
            with self.subTest(tail=tail):
                run = subprocess.run([HELPER, '--permissions'], input=first+tail,
                                     capture_output=True, timeout=5)
                self.assertEqual(run.returncode, 1)
                self.assertEqual(len(run.stdout), 344)  # first query only
                self.assertEqual(HEAD.unpack(run.stdout[28:40]), (22, 304, 1))
                self.assertEqual(self.item.stat().st_mode & 0o7777, 0o640)

if __name__ == '__main__':
    unittest.main()
