"""Native read-only hash protocol contracts. Run as the unprivileged user."""
import errno
import hashlib
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

HELPER = sys.argv.pop(1)
HEAD = struct.Struct('<III')
RESULT = struct.Struct('<Qq32sII')

class FileHash(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='openpilot_hash_')
        self.root = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def hash(self, path, stop=False, changed=False):
        raw = os.fsencode(path)
        p = subprocess.Popen([HELPER, '--hash'], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        try:
            self.assertEqual(HEAD.unpack(p.stdout.read(12)), (1, 16, 0))
            self.assertEqual(len(p.stdout.read(16)), 16)
            p.stdin.write(HEAD.pack(41, len(raw), 1) + raw)
            if stop:
                p.stdin.write(b'cancel')
            p.stdin.flush()
            while True:
                self.assertEqual(HEAD.unpack(p.stdout.read(12)), (42, 56, 1))
                r = RESULT.unpack(p.stdout.read(56))
                if r[-1]:
                    break
                if changed:
                    os.utime(path, ns=(1_700_000_000_000_000_000, 1_700_000_000_000_000_000))
                    changed = False
            self.assertEqual(p.wait(timeout=10), 0)
            self.assertEqual(p.stdout.read(), b'')
            return r
        finally:
            p.stdin.close(); p.stdout.close()
            if p.poll() is None:
                p.kill(); p.wait()

    def test_known_empty_and_multiblock_digests(self):
        for payload in (b'', b'abc', bytes(range(256))*513):
            f = self.root/'data'; f.write_bytes(payload)
            r = self.hash(f)
            self.assertEqual(r, (len(payload), f.stat().st_mtime_ns//100 + 116444736000000000,
                                 hashlib.sha256(payload).digest(), 0, 1))

    def test_literal_reserved_unicode_and_raw_byte_names(self):
        for name in ('a:b*$(literal);💀', b'raw_\xff'):
            p = os.fsencode(self.root) + b'/' + os.fsencode(name)
            with open(p, 'wb') as f:
                f.write(b'literal')
            self.assertEqual(self.hash(p)[2:4], (hashlib.sha256(b'literal').digest(), 0))

    def test_links_directories_and_fifos_never_read(self):
        f = self.root/'data'; f.write_bytes(b'secret')
        (self.root/'link').symlink_to(f)
        os.mkfifo(self.root/'fifo')
        for p in (self.root/'link', self.root/'fifo', self.root):
            r = self.hash(p)
            self.assertNotEqual(r[-2], 0)
            self.assertEqual(r[2], bytes(32))
        self.assertEqual(f.read_bytes(), b'secret')

    def test_cancel_never_produces_a_valid_digest(self):
        f = self.root/'data'; f.write_bytes(b'abc')
        self.assertEqual(self.hash(f, stop=True)[2:], (bytes(32), errno.ECANCELED, 1))

    def test_changed_during_read_is_unverified(self):
        f = self.root/'large'
        with f.open('wb') as out:
            out.truncate(128*1024*1024)
        r = self.hash(f, changed=True)
        self.assertEqual(r[2:], (bytes(32), errno.ESTALE, 1))

    def test_missing_and_permission_denied(self):
        self.assertNotEqual(os.geteuid(), 0)
        self.assertEqual(self.hash(self.root/'absent')[-2], errno.ENOENT)
        f = self.root/'denied'; f.write_bytes(b'secret'); f.chmod(0)
        try:
            self.assertEqual(self.hash(f)[-2], errno.EACCES)
        finally:
            f.chmod(0o600)

    def test_malformed_requests_are_rejected(self):
        for request in (HEAD.pack(41, 0, 1), HEAD.pack(41, 16385, 1),
                        HEAD.pack(41, 4, 1)+b'/a\0b', HEAD.pack(41, 3, 2)+b'/ab'):
            r = subprocess.run([HELPER, '--hash'], input=request, capture_output=True, timeout=5)
            self.assertNotEqual(r.returncode, 0)
            self.assertEqual(len(r.stdout), 28)
        self.assertEqual(self.hash('relative')[-2], errno.EINVAL)

if __name__ == '__main__':
    unittest.main(verbosity=2)
