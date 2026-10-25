"""Native Linux bulk rename: preview identity, literal paths, no replacement."""
import errno
import os
from pathlib import Path
import select
import struct
import subprocess
import sys
import tempfile
import time
import unittest

HELPER = sys.argv.pop(1)
HEAD = struct.Struct('<III')
IDENTITY = struct.Struct('<QQQII')
RESULT = struct.Struct('<QQQIIII')
BATCH, PROGRESS, REPLY, CANCEL = 37, 38, 39, 40

class BulkRename(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='openpilot_rename_')
        self.root = Path(self.tmp.name)
        (self.root / 'a.txt').write_bytes(b'alpha')
        (self.root / 'b.txt').write_bytes(b'beta')

    def tearDown(self):
        self.tmp.cleanup()

    def read(self, p, count):
        deadline = time.monotonic() + 10
        out = b''
        while len(out) < count:
            ready = select.select([p.stdout], [], [], max(0, deadline-time.monotonic()))[0]
            self.assertTrue(ready, 'Native rename reply timed out')
            block = os.read(p.stdout.fileno(), count-len(out))
            self.assertTrue(block, 'Native rename reply truncated')
            out += block
        return out

    def call(self, pairs, apply=False, expected=None, stop=False, eof=False, root=None):
        raw_root = os.fsencode(root or self.root)
        payload = struct.pack('<IIII', int(apply), len(pairs), len(raw_root), 0) + raw_root
        for i, (before, after) in enumerate(pairs):
            before, after = os.fsencode(before), os.fsencode(after)
            identity = IDENTITY.pack(*expected[i][:5]) if expected else bytes(32)
            payload += identity + struct.pack('<II', len(before), len(after)) + before + after
        data = HEAD.pack(BATCH, len(payload), 1) + payload
        if stop:
            data += HEAD.pack(CANCEL, 0, 1)
        # EOF must already be observable when the helper reads the batch. A
        # parent close after writing races with a complete, valid Apply.
        queued = None
        if eof:
            reader, writer = os.pipe()
            try:
                self.assertLessEqual(len(data), os.fpathconf(writer, 'PC_PIPE_BUF'))
                self.assertEqual(os.write(writer, data), len(data))
                queued = os.fdopen(reader, 'rb', buffering=0)
            except BaseException:
                os.close(reader)
                raise
            finally:
                os.close(writer)
        try:
            p = subprocess.Popen([HELPER, '--rename'], stdin=queued if eof else subprocess.PIPE,
                                 stdout=subprocess.PIPE, bufsize=0)
        finally:
            if queued:
                queued.close()
        try:
            self.assertEqual(HEAD.unpack(self.read(p, 12)), (1, 16, 0)); self.read(p, 16)
            if not eof:
                p.stdin.write(data)
            progress = []
            while True:
                kind, size, token = HEAD.unpack(self.read(p, 12)); self.assertEqual(token, 1)
                body = self.read(p, size)
                if kind == PROGRESS:
                    self.assertEqual(size, 8); progress.append(struct.unpack('<II', body)); continue
                self.assertEqual(kind, REPLY)
                count, stopped = struct.unpack('<II', body[:8]); self.assertEqual(count, len(pairs))
                self.assertEqual(size, 8 + count*40)
                results = [RESULT.unpack_from(body, 8+i*40) for i in range(count)]
                break
            self.assertEqual(p.wait(timeout=5), 0)
            self.assertEqual(p.stdout.read(), b'')
            return results, stopped, progress
        finally:
            if p.stdin and not p.stdin.closed:
                p.stdin.close()
            p.stdout.close()
            if p.poll() is None:
                p.kill(); p.wait()

    def test_preview_is_read_only_and_apply_is_explicit(self):
        pairs = [('a.txt', 'first.txt'), ('b.txt', 'second.txt')]
        before = (self.root/'a.txt').stat()
        draft, _, progress = self.call(pairs)
        self.assertEqual([r[-2:] for r in draft], [(1, 0), (1, 0)])
        self.assertEqual(progress, [(1, 0), (2, 0)])
        self.assertTrue((self.root/'a.txt').exists()); self.assertFalse((self.root/'first.txt').exists())
        self.assertEqual((self.root/'a.txt').stat().st_ctime_ns, before.st_ctime_ns)
        result, stopped, _ = self.call(pairs, apply=True, expected=draft)
        self.assertEqual([r[-2:] for r in result], [(3, 0), (3, 0)]); self.assertEqual(stopped, 0)
        self.assertEqual((self.root/'first.txt').read_bytes(), b'alpha')
        self.assertEqual((self.root/'second.txt').read_bytes(), b'beta')

    def test_existing_destination_blocks_the_entire_apply(self):
        pairs = [('a.txt', 'first.txt'), ('b.txt', 'second.txt')]
        draft = self.call(pairs)[0]; (self.root/'second.txt').write_bytes(b'keep')
        result = self.call(pairs, apply=True, expected=draft)[0]
        self.assertEqual(result[0][-2:], (6, 0)); self.assertEqual(result[1][-2:], (4, errno.EEXIST))
        self.assertEqual((self.root/'second.txt').read_bytes(), b'keep'); self.assertTrue((self.root/'a.txt').exists())
        self.assertFalse((self.root/'first.txt').exists())

    def test_changed_or_replaced_source_invalidates_review(self):
        pairs = [('a.txt', 'first.txt'), ('b.txt', 'second.txt')]
        draft = self.call(pairs)[0]
        (self.root/'b.txt').rename(self.root/'old'); (self.root/'b.txt').write_bytes(b'replacement')
        result = self.call(pairs, apply=True, expected=draft)[0]
        self.assertEqual(result[1][-2:], (4, errno.ESTALE)); self.assertEqual(result[0][-2:], (6, 0))
        self.assertTrue((self.root/'a.txt').exists()); self.assertEqual((self.root/'b.txt').read_bytes(), b'replacement')

    def test_duplicates_and_selected_occupied_names_are_conflicts(self):
        result = self.call([('a.txt', 'same.txt'), ('b.txt', 'same.txt')])[0]
        self.assertEqual([r[-2:] for r in result], [(4, errno.EEXIST), (4, errno.EEXIST)])
        result = self.call([('a.txt', 'b.txt'), ('b.txt', 'a.txt')])[0]
        self.assertEqual([r[-2:] for r in result], [(4, errno.EEXIST), (4, errno.EEXIST)])
        os.link(self.root/'a.txt', self.root/'hard.txt')
        self.assertEqual(self.call([('a.txt', 'hard.txt')])[0][0][-2:], (4, errno.EEXIST))

    def test_unchanged_and_case_sensitive_names(self):
        (self.root/'A.txt').write_bytes(b'upper')
        result = self.call([('a.txt', 'A.txt')])[0]; self.assertEqual(result[0][-2:], (4, errno.EEXIST))
        pairs = [('a.txt', 'a.txt'), ('b.txt', 'B.txt')]; draft = self.call(pairs)[0]
        result = self.call(pairs, apply=True, expected=draft)[0]
        self.assertEqual([r[-2:] for r in result], [(2, 0), (3, 0)])
        self.assertTrue((self.root/'A.txt').exists()); self.assertTrue((self.root/'a.txt').exists())

    def test_literal_reserved_unicode_and_raw_byte_names(self):
        for name in ['β:star*$(literal);', '💀 rocket.txt', b'raw\xff']:
            path = os.fsencode(self.root) + b'/' + os.fsencode(name)
            with open(path, 'wb') as f:
                f.write(b'keep')
            target = os.fsencode(name) + b'-new'
            pairs = [(name, target)]; draft = self.call(pairs)[0]
            self.assertEqual(self.call(pairs, apply=True, expected=draft)[0][0][-2:], (3, 0))
            with open(os.fsencode(self.root)+b'/'+target, 'rb') as f:
                self.assertEqual(f.read(), b'keep')

    def test_folders_links_and_contents_are_not_followed_or_renamed(self):
        (self.root/'folder.txt').mkdir(); (self.root/'folder.txt'/'inside').write_bytes(b'inside')
        (self.root/'link.txt').symlink_to('missing')
        pairs = [('folder.txt', 'folder-new'), ('link.txt', 'link-new')]; draft = self.call(pairs)[0]
        self.assertEqual(draft[0][4], 0x10); self.assertEqual(draft[1][4], 0x400)
        result = self.call(pairs, apply=True, expected=draft)[0]
        self.assertEqual([r[-2:] for r in result], [(3, 0), (3, 0)])
        self.assertEqual((self.root/'folder-new'/'inside').read_bytes(), b'inside')
        self.assertEqual(os.readlink(self.root/'link-new'), 'missing')

    def test_cancel_or_caller_disconnect_does_not_start_mutations(self):
        pairs = [('a.txt', 'first.txt'), ('b.txt', 'second.txt')]; draft = self.call(pairs)[0]
        for option in ['stop', 'eof']:
            with self.subTest(cancellation=option):
                result, stopped, _ = self.call(pairs, apply=True, expected=draft, **{option: True})
                self.assertEqual(stopped, 1); self.assertEqual([r[-2:] for r in result], [(5, 0), (5, 0)])
                self.assertTrue((self.root/'a.txt').exists()); self.assertFalse((self.root/'first.txt').exists())
        # With stdin left open until after the reply and process exit, Apply
        # legitimately finishes. Closing the caller cannot undo that mutation.
        result, stopped, _ = self.call(pairs, apply=True, expected=draft)
        self.assertEqual(stopped, 0); self.assertEqual([r[-2:] for r in result], [(3, 0), (3, 0)])
        self.assertEqual((self.root/'first.txt').read_bytes(), b'alpha')
        self.assertEqual((self.root/'second.txt').read_bytes(), b'beta')

    def test_permission_failure_is_reported_without_elevation(self):
        self.assertNotEqual(os.geteuid(), 0, 'Run with the default unprivileged WSL user')
        pairs = [('a.txt', 'first.txt'), ('b.txt', 'second.txt')]; draft = self.call(pairs)[0]
        self.root.chmod(0o500)
        try:
            result = self.call(pairs, apply=True, expected=draft)[0]
            self.assertEqual([r[-2:] for r in result], [(4, errno.EACCES), (4, errno.EACCES)])
            self.assertTrue((self.root/'a.txt').exists()); self.assertFalse((self.root/'first.txt').exists())
        finally:
            self.root.chmod(0o700)

    def test_missing_parent_or_source_is_an_item_error(self):
        result = self.call([('missing', 'new')])[0]
        self.assertEqual(result[0][-2:], (4, errno.ENOENT))
        result = self.call([('a.txt', 'new')], root=self.root/'missing')[0]
        self.assertEqual(result[0][-2:], (4, errno.ENOENT))

    def test_maximum_selection_has_bounded_results(self):
        pairs = []
        for i in range(256):
            name = f'item{i}'; (self.root/name).touch(); pairs.append((name, name+'-new'))
        draft = self.call(pairs)[0]; self.assertEqual(len(draft), 256)
        result = self.call(pairs, apply=True, expected=draft)[0]
        self.assertTrue(all(r[-2:] == (3, 0) for r in result))

    def test_invalid_components_and_malformed_packets_are_rejected(self):
        root = os.fsencode(self.root)
        for before, after in [(b'a.txt', b'../outside'), (b'a.txt', b'a\0b'), (b'a.txt', b''),
                              (b'a.txt', b'x'*256), (b'..', b'new')]:
            payload = struct.pack('<IIII', 0, 1, len(root), 0) + root + bytes(32)
            payload += struct.pack('<II', len(before), len(after)) + before + after
            p = subprocess.run([HELPER, '--rename'], input=HEAD.pack(BATCH, len(payload), 1)+payload, capture_output=True, timeout=5)
            self.assertNotEqual(p.returncode, 0); self.assertEqual(len(p.stdout), 28)
        for payload in [struct.pack('<IIII', 0, 0, 1, 0)+b'/', struct.pack('<IIII', 0, 257, 1, 0)+b'/', struct.pack('<IIII', 2, 1, 1, 0)+b'/']:
            p = subprocess.run([HELPER, '--rename'], input=HEAD.pack(BATCH, len(payload), 1)+payload, capture_output=True, timeout=5)
            self.assertNotEqual(p.returncode, 0)
        self.assertTrue((self.root/'a.txt').exists())

if __name__ == '__main__':
    unittest.main(verbosity=2)
