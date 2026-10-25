"""Guarded Linux ownership and read-only identity resolution; private fixtures."""
import errno
import grp
import os
from pathlib import Path
import pwd
import struct
import subprocess
import sys
import tempfile
import unittest

HELPER = sys.argv.pop(1)
HEAD = struct.Struct('<III')
STATE = struct.Struct('<QQqIIII')

class Ownership(unittest.TestCase):
    def setUp(self):
        self.assertEqual(os.geteuid(), 0, 'Run this private contract gate with WSL --user root')
        self.tmp = tempfile.TemporaryDirectory(prefix='openpilot_ownership_')
        self.root = Path(self.tmp.name)
        self.item = self.root / 'file β:star*$(literal);'
        self.item.write_bytes(b'unchanged content')
        self.item.chmod(0o640)

    def tearDown(self):
        self.tmp.cleanup()

    def exchange(self, kind, payload, answer, size, uid=None):
        def demote():
            os.setgroups([]); os.setgid(uid); os.setuid(uid)
        run = subprocess.run([HELPER, '--permissions'], input=HEAD.pack(kind, len(payload), 7)+payload,
                             capture_output=True, timeout=10, preexec_fn=demote if uid is not None else None)
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(HEAD.unpack(run.stdout[:12]), (1, 16, 0))
        self.assertEqual(HEAD.unpack(run.stdout[28:40]), (answer, size, 7))
        self.assertEqual(len(run.stdout), 40+size)
        return run.stdout[40:]

    def resolve(self, owner='', group='', fields=None):
        chosen = int(bool(owner)) | (int(bool(group)) << 1) if fields is None else fields
        payload = struct.pack('<II128s128s', chosen, 0, owner.encode(), group.encode())
        reply = self.exchange(23, payload, 24, 280)
        return struct.unpack('<iIIIII', reply[:24]), reply[24:152].split(b'\0')[0], reply[152:].split(b'\0')[0]

    def query(self, path=None, uid=None):
        raw = os.fsencode(path or self.item)
        reply = self.exchange(21, struct.pack('<IIII', 0, 0, len(raw), 0)+bytes(40)+raw, 22, 304, uid)
        self.assertEqual(struct.unpack('<iI', reply[:8]), (0, 0))
        return reply[8:48]

    def apply(self, owner=0, group=0, fields=3, expected=None, path=None, uid=None):
        raw = os.fsencode(path or self.item)
        meta = self.query(path, uid) if expected is None else expected
        reply = self.exchange(25, struct.pack('<IIII', fields, owner, group, len(raw))+meta+raw, 26, 304, uid)
        return struct.unpack('<iI', reply[:8]), STATE.unpack(reply[8:48])

    def test_resolve_names_and_decimal_ids_is_read_only(self):
        before = self.item.stat()
        values, owner, group = self.resolve(pwd.getpwuid(0).pw_name, grp.getgrgid(0).gr_name)
        self.assertEqual(values, (0, 0, 3, 0, 0, 0)); self.assertTrue(owner); self.assertTrue(group)
        values, owner, group = self.resolve('000', '0')
        self.assertEqual(values, (0, 0, 3, 0, 0, 0))
        self.assertEqual(self.item.stat().st_ctime_ns, before.st_ctime_ns)

    def test_unmapped_numeric_ids_and_keep_fields(self):
        values, _, _ = self.resolve('4294967294')
        self.assertEqual(values, (0, 0, 1, 4294967294, 0, 0))
        values, _, _ = self.resolve(group='65534')
        self.assertEqual(values[:5], (0, 0, 2, 0, 65534))

    def test_invalid_unknown_and_literal_identity_inputs(self):
        injection = self.root/'injection'
        for name in ['-1', '+0', '4294967295', '4294967296', '999999999999999999999999999999', 'root\n', 'root\t', 'not_a_user_openpilot', f'$(touch {injection})']:
            result = self.resolve(name)[0]
            self.assertNotEqual(result[0], 0, name); self.assertEqual(result[1], 1)
        result = self.resolve(group='not_a_group_openpilot')[0]
        self.assertEqual(result[:2], (errno.ENOENT, 2))
        self.assertEqual(self.resolve(fields=0)[0][0], errno.EINVAL)
        self.assertEqual(self.resolve('root', fields=4)[0][0], errno.EINVAL)
        self.assertFalse(injection.exists())

    def test_owner_only_literal_filename_retains_group_content_and_mode(self):
        result, meta = self.apply(owner=65534, fields=1)
        self.assertEqual(result, (0, 1)); self.assertEqual(meta[5:], (65534, 0))
        self.assertEqual(self.item.stat().st_mode & 0o7777, 0o640)
        self.assertEqual(self.item.read_bytes(), b'unchanged content')

    def test_group_only_retains_owner_and_mode(self):
        result, meta = self.apply(group=65534, fields=2)
        self.assertEqual(result, (0, 1)); self.assertEqual(meta[5:], (0, 65534))
        self.assertEqual(self.item.stat().st_mode & 0o7777, 0o640)

    def test_both_fields_report_actual_cleared_special_bits_without_chmod(self):
        self.item.chmod(0o6755)
        result, meta = self.apply(owner=65534, group=65534)
        self.assertEqual(result, (0, 1)); self.assertEqual(meta[5:], (65534, 65534))
        self.assertEqual(meta[4] & 0o7777, 0o755)
        self.assertEqual(self.item.stat().st_mode & 0o7777, 0o755)

    def test_noop_preserves_ctime_special_bits_and_capabilities(self):
        self.item.chmod(0o6755)
        os.setxattr(self.item, 'security.capability', struct.pack('<IIIII', 0x02000001, 1 << 10, 0, 0, 0))
        cap = os.getxattr(self.item, 'security.capability'); before = self.item.stat()
        result, meta = self.apply()
        self.assertEqual(result, (0, 0)); self.assertEqual(meta[4] & 0o7777, 0o6755)
        self.assertEqual(self.item.stat().st_ctime_ns, before.st_ctime_ns)
        self.assertEqual(os.getxattr(self.item, 'security.capability'), cap)
        self.assertEqual(self.apply(owner=65534, fields=1)[0], (0, 1))
        with self.assertRaises(OSError) as error: os.getxattr(self.item, 'security.capability')
        self.assertEqual(error.exception.errno, errno.ENODATA)

    def test_directory_is_nonrecursive_and_unreadable_files_can_change_owner(self):
        folder = self.root/'directory'; folder.mkdir(); nested = folder/'nested'; nested.write_text('keep'); nested.chmod(0o640)
        folder.chmod(0o2750)
        self.assertEqual(self.apply(owner=65534, fields=1, path=folder)[0], (0, 1))
        self.assertEqual(folder.stat().st_mode & 0o7777, 0o2750)
        self.assertEqual(nested.stat().st_uid, 0); self.assertEqual(nested.stat().st_mode & 0o7777, 0o640)
        self.item.chmod(0)
        self.assertEqual(self.apply(owner=65534, fields=1)[0], (0, 1))
        self.assertEqual(self.item.stat().st_mode & 0o7777, 0)

    def test_stale_mode_owner_and_replacement_are_not_changed(self):
        meta = self.query(); self.item.chmod(0o600)
        self.assertEqual(self.apply(owner=65534, fields=1, expected=meta)[0], (errno.ESTALE, 0))
        self.assertEqual(self.item.stat().st_uid, 0)
        meta = self.query(); os.chown(self.item, 65534, 0)
        self.assertEqual(self.apply(owner=0, fields=1, expected=meta)[0], (errno.ESTALE, 0))
        self.assertEqual(self.item.stat().st_uid, 65534)
        meta = self.query(); self.item.rename(self.root/'original'); self.item.write_text('replacement')
        self.assertEqual(self.apply(owner=65534, fields=1, expected=meta)[0], (errno.ESTALE, 0))
        self.assertEqual(self.item.stat().st_uid, 0)

    def test_symlinks_trailing_directory_links_special_and_missing_are_refused(self):
        meta = self.query(); link = self.root/'link'; link.symlink_to(self.item)
        folder = self.root/'folder'; folder.mkdir(); dlink = self.root/'dlink'; dlink.symlink_to(folder)
        fifo = self.root/'fifo'; os.mkfifo(fifo)
        for path in [link, str(dlink)+'/', fifo]:
            self.assertEqual(self.apply(owner=65534, fields=1, path=path, expected=meta)[0], (errno.EOPNOTSUPP, 0))
        self.assertEqual(self.apply(path=self.root/'missing', expected=meta)[0], (errno.ENOENT, 0))
        self.assertEqual(self.item.stat().st_uid, 0)

    def test_denial_and_owner_allowed_group_change_have_no_elevation(self):
        self.root.chmod(0o755)
        before = self.item.stat()
        self.assertEqual(self.apply(owner=65534, fields=1, uid=65534)[0], (errno.EPERM, 0))
        self.assertEqual(self.item.stat().st_ctime_ns, before.st_ctime_ns)
        os.chown(self.item, 65534, 0); self.item.chmod(0o2750)
        self.assertEqual(self.apply(group=65534, fields=2, uid=65534)[0], (0, 1))
        self.assertEqual(self.item.stat().st_uid, 65534); self.assertEqual(self.item.stat().st_gid, 65534)
        self.assertEqual(self.item.stat().st_mode & 0o7777, 0o750)
        self.assertEqual(self.apply(owner=0, fields=1, uid=65534)[0], (errno.EPERM, 0))
        self.assertEqual(self.item.stat().st_uid, 65534)

    def test_invalid_flags_sentinels_and_malformed_packets_never_mutate(self):
        meta = self.query(); before = self.item.stat()
        for fields, owner, group in [(0, 0, 0), (4, 0, 0), (1, 0xffffffff, 0), (2, 0, 0xffffffff), (1, 0, 1), (2, 1, 0)]:
            self.assertEqual(self.apply(owner, group, fields, meta)[0], (errno.EINVAL, 0))
        for raw in [b'', b'/tmp\0injected']:
            payload = struct.pack('<IIII', 1, 65534, 0, len(raw))+meta+raw
            run = subprocess.run([HELPER, '--permissions'], input=HEAD.pack(25, len(payload), 1)+payload, capture_output=True, timeout=10)
            self.assertEqual(run.returncode, 1); self.assertEqual(len(run.stdout), 28)
        self.assertEqual(self.item.stat().st_ctime_ns, before.st_ctime_ns)

if __name__ == '__main__':
    unittest.main()
