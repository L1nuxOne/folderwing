"""Native read-only search traversal: literal bytes, metadata and boundaries."""
import errno
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

HELPER = sys.argv.pop(1)
HEAD = struct.Struct('<III')
ENTRY = struct.Struct('<QqII')
END = struct.Struct('<IIII')

class FolderSearch(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='openpilot_search_')
        self.root = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def scan(self, path=None, stop=False):
        p = subprocess.Popen([HELPER, '--search'], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        try:
            self.assertEqual(HEAD.unpack(p.stdout.read(12)), (1, 16, 0))
            self.assertEqual(len(p.stdout.read(16)), 16)
            raw = os.fsencode(path or self.root)
            p.stdin.write(HEAD.pack(45, len(raw), 1) + raw)
            if stop:
                p.stdin.write(HEAD.pack(999, 0, 1))
            p.stdin.flush()
            rows = []
            while True:
                kind, size, ident = HEAD.unpack(p.stdout.read(12))
                self.assertEqual(ident, 1)
                payload = p.stdout.read(size)
                self.assertEqual(len(payload), size)
                if kind == 47:
                    self.assertEqual(size, END.size)
                    result = END.unpack(payload)
                    break
                self.assertEqual(kind, 46)
                item = ENTRY.unpack_from(payload)
                self.assertEqual(item[3], size - ENTRY.size)
                rows.append((payload[ENTRY.size:], *item[:3]))
            self.assertEqual(p.wait(timeout=5), 0)
            self.assertEqual(p.stdout.read(), b'')
            return rows, result
        finally:
            p.stdin.close()
            p.stdout.close()
            if p.poll() is None:
                p.kill()
                p.wait()

    def test_literal_raw_unicode_and_nested_names(self):
        nested = self.root / 'nested'; nested.mkdir()
        names = ['report β:star*$(literal);', 'back\\slash', '.hidden']
        for name in names:
            (nested / name).write_bytes(b'abc')
        raw = os.fsencode(nested) + b'/raw\xff'
        fd = os.open(raw, os.O_CREAT | os.O_WRONLY, 0o600); os.close(fd)
        rows, result = self.scan()
        self.assertEqual(result, (0, 5, 0, 0))
        found = {row[0]: row for row in rows}
        for name in names:
            row = found[b'nested/' + os.fsencode(name)]
            self.assertEqual(row[1], 3)
            self.assertEqual(row[3], 0x80)
        self.assertIn(b'nested/raw\xff', found)
        self.assertEqual(found[b'nested'][3], 0x10)

    def test_links_and_special_nodes_are_listed_without_descent(self):
        outside = self.root / 'outside'; outside.mkdir()
        (outside / 'secret').write_text('leave')
        link = self.root / 'link'; link.symlink_to(outside, target_is_directory=True)
        os.mkfifo(self.root / 'fifo')
        rows, result = self.scan()
        self.assertEqual(result, (0, 4, 2, 0))
        found = {row[0]: row for row in rows}
        self.assertEqual(found[b'link'][3], 0x400)
        self.assertEqual(found[b'fifo'][3], 0x400)
        self.assertNotIn(b'link/secret', found)

    def test_empty_missing_relative_and_link_roots(self):
        self.assertEqual(self.scan(), ([], (0, 0, 0, 0)))
        self.assertEqual(self.scan(self.root / 'absent')[1][0], errno.ENOENT)
        self.assertEqual(self.scan('relative')[1][0], errno.EINVAL)
        (self.root / 'link').symlink_to(self.root, target_is_directory=True)
        self.assertIn(self.scan(self.root / 'link')[1][0], (errno.ELOOP, errno.ENOTDIR))

    def test_cancel_queued_before_scan_does_no_traversal(self):
        (self.root / 'file').write_text('keep')
        rows, result = self.scan(stop=True)
        self.assertEqual(rows, [])
        self.assertEqual(result, (errno.ECANCELED, 0, 0, 1))
        self.assertEqual((self.root / 'file').read_text(), 'keep')

    def test_depth_limit_reports_incomplete_and_never_reaches_leaf(self):
        here = self.root
        for _ in range(130):
            here = here / 'd'; here.mkdir()
        (here / 'unreached').write_text('leave')
        rows, result = self.scan()
        self.assertEqual(result[0], errno.EOVERFLOW)
        self.assertEqual(result[1], 128)
        self.assertFalse(any(row[0].endswith(b'unreached') for row in rows))

    def test_permission_denial_is_partial_without_elevation(self):
        self.assertNotEqual(os.geteuid(), 0, 'contract requires the normal Linux user')
        blocked = self.root / 'blocked'; blocked.mkdir(); (blocked / 'secret').write_text('leave')
        blocked.chmod(0)
        try:
            rows, result = self.scan()
            self.assertEqual(result, (errno.EACCES, 1, 0, 1))
            self.assertEqual([row[0] for row in rows], [b'blocked'])
        finally:
            blocked.chmod(0o700)

    def test_malformed_requests_exit_without_streaming_entries(self):
        for request in (HEAD.pack(45, 0, 1), HEAD.pack(45, 16385, 1),
                        HEAD.pack(45, 2, 1) + b'/\0', HEAD.pack(44, 1, 1) + b'/',
                        HEAD.pack(45, 9, 1) + b'/short'):
            p = subprocess.run([HELPER, '--search'], input=request, capture_output=True, timeout=5)
            self.assertNotEqual(p.returncode, 0)
            self.assertEqual(len(p.stdout), 28)

if __name__ == '__main__':
    unittest.main(verbosity=2)
