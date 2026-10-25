"""Read-only native Linux sizing: literal names, bounded recursion and stops."""
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
SIZE = struct.Struct('<QQQQQII')

class FolderSizes(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='openpilot_sizes_')
        self.root = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def scan(self, path=None, stop=False, eof=False):
        raw = os.fsencode(path or self.root)
        request = HEAD.pack(34, len(raw), 1) + raw
        if eof:
            # Queue a complete request and EOF before the helper starts. Closing
            # after flush can race a fast successful scan and is not a stop.
            read_fd, write_fd = os.pipe()
            try:
                self.assertEqual(os.write(write_fd, request), len(request))
            finally:
                os.close(write_fd)
            try:
                p = subprocess.Popen([HELPER, '--sizes'], stdin=read_fd, stdout=subprocess.PIPE)
            finally:
                os.close(read_fd)
        else:
            p = subprocess.Popen([HELPER, '--sizes'], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        try:
            self.assertEqual(HEAD.unpack(p.stdout.read(12)), (1, 16, 0))
            self.assertEqual(len(p.stdout.read(16)), 16)
            if not eof:
                p.stdin.write(request)
                if stop:
                    p.stdin.write(HEAD.pack(36, 0, 1))
                p.stdin.flush()
            values = []
            while True:
                self.assertEqual(HEAD.unpack(p.stdout.read(12)), (35, 48, 1))
                r = SIZE.unpack(p.stdout.read(48)); values.append(r)
                if r[-1]:
                    break
            self.assertEqual(p.wait(timeout=5), 0)
            self.assertEqual(p.stdout.read(), b'')
            for a, b in zip(values, values[1:]):
                self.assertTrue(all(x <= y for x, y in zip(a[:5], b[:5])))
            return values[-1]
        finally:
            if p.stdin is not None and not p.stdin.closed:
                p.stdin.close()
            p.stdout.close()
            if p.poll() is None:
                p.kill(); p.wait()

    def test_nested_hidden_empty_and_literal_names(self):
        (self.root / 'empty').mkdir(); nested = self.root / 'nested'; nested.mkdir()
        (self.root / '.hidden').write_bytes(b'abc')
        (nested / 'β:star*$(literal);').write_bytes(b'1234567')
        before = (nested / 'β:star*$(literal);').stat()
        self.assertEqual(self.scan(), (10, 2, 3, 0, 0, 0, 1))
        after = (nested / 'β:star*$(literal);').stat()
        self.assertEqual((after.st_mtime_ns, after.st_ctime_ns, after.st_atime_ns),
                         (before.st_mtime_ns, before.st_ctime_ns, before.st_atime_ns))

    def test_sparse_logical_bytes_and_hard_link_entries(self):
        f = self.root / 'large'
        with f.open('wb') as out:
            out.truncate(5 * 1024**3 + 7)
        os.link(f, self.root / 'again')
        self.assertEqual(self.scan()[:3], (2 * (5 * 1024**3 + 7), 2, 1))

    def test_links_loops_broken_links_and_fifo_are_excluded(self):
        (self.root / 'file').write_bytes(b'abc')
        (self.root / 'loop').symlink_to(self.root)
        (self.root / 'outside').symlink_to('/etc')
        (self.root / 'broken').symlink_to('missing')
        os.mkfifo(self.root / 'fifo')
        self.assertEqual(self.scan(), (3, 1, 1, 4, 0, 0, 1))
        self.assertEqual(self.scan(str(self.root / 'loop') + '/')[-1], 2)

    def test_missing_and_regular_roots_are_partial(self):
        r = self.scan(self.root / 'missing')
        self.assertEqual(r[4:], (1, errno.ENOENT, 2))
        (self.root / 'file').touch()
        self.assertEqual(self.scan(self.root / 'file')[4:], (1, errno.ENOTDIR, 2))

    def test_unreadable_subtree_retains_other_totals(self):
        self.assertNotEqual(os.geteuid(), 0, 'Run as the default unprivileged WSL user')
        denied = self.root / 'denied'; denied.mkdir(); (denied / 'secret').write_bytes(b'hidden')
        (self.root / 'visible').write_bytes(b'visible'); denied.chmod(0)
        try:
            self.assertEqual(self.scan(), (7, 1, 1, 0, 1, errno.EACCES, 2))
        finally:
            denied.chmod(0o700)

    def test_cancel_and_disconnected_caller_are_incomplete(self):
        for i in range(100):
            (self.root / str(i)).write_bytes(b'x')
        self.assertEqual(self.scan(stop=True)[-1], 3)
        self.assertEqual(self.scan(eof=True)[-1], 3)
        self.assertEqual(self.scan()[0], 100)

    def test_depth_limit_is_partial_and_finite(self):
        d = self.root
        for _ in range(131):
            d = d / 'd'; d.mkdir()
        r = self.scan()
        self.assertEqual(r[2], 129)
        self.assertEqual(r[4:], (1, errno.EOVERFLOW, 2))

    def test_recalculation_reads_fresh_metadata(self):
        self.assertEqual(self.scan()[:3], (0, 0, 1))
        f = self.root / 'changing'; f.write_bytes(b'hello')
        self.assertEqual(self.scan()[0], 5)
        f.write_bytes(b'changed content')
        self.assertEqual(self.scan()[0], 15)
        f.unlink(); self.assertEqual(self.scan()[0], 0)

    def test_malformed_protocol_is_rejected(self):
        for payload in (HEAD.pack(34, 0, 1), HEAD.pack(34, 16385, 1),
                        HEAD.pack(34, 4, 1) + b'/a\0b', HEAD.pack(34, 3, 2) + b'/ab'):
            r = subprocess.run([HELPER, '--sizes'], input=payload, capture_output=True, timeout=5)
            self.assertNotEqual(r.returncode, 0)
            self.assertEqual(len(r.stdout), 28)
        self.assertEqual(self.scan('relative')[4:], (1, errno.EINVAL, 2))

if __name__ == '__main__':
    unittest.main(verbosity=2)
