"""Pinned recursive-directory and regular-only copy helper contracts."""
import errno
import json
import os
from pathlib import Path
import select
import struct
import subprocess
import sys
import tempfile
import unittest

HELPER = sys.argv.pop(1)
ROOT = Path(__file__).resolve().parents[1]
HEAD = struct.Struct('<III')

TEST_CONTRACTS = {'test_mount_identity_fallback_preserves_normal_children_and_link_refusal': {'owner': 'Pinned '
                                                                                      'Linux '
                                                                                      'comparison '
                                                                                      'descendant '
                                                                                      'mount scope '
                                                                                      'and '
                                                                                      'fail-closed '
                                                                                      'identity',
                                                                             'primary': 'Linux '
                                                                                        'integration',
                                                                             'sources': ['linux/fpguard.c',
                                                                                         'linux/fpmount.h',
                                                                                         'linux/fpancestry.h',
                                                                                         'src/pathguard.c',
                                                                                         'src/comparetree.c',
                                                                                         'src/wsl.c',
                                                                                         'src/wslproto.h',
                                                                                         'tests/test_tree_guard.py',
                                                                                         'tests/fixtures/guard_durability.c',
                                                                                         'tests/fixtures/guard_mounts.py',
                                                                                         'tests/fixtures/no_openat2.py']},
 'test_mount_identity_unavailable_fails_closed': {'owner': 'Pinned Linux comparison descendant '
                                                           'mount scope and fail-closed identity',
                                                  'primary': 'Linux integration',
                                                  'sources': ['linux/fpguard.c',
                                                              'linux/fpmount.h',
                                                              'linux/fpancestry.h',
                                                              'src/pathguard.c',
                                                              'src/comparetree.c',
                                                              'src/wsl.c',
                                                              'src/wslproto.h',
                                                              'tests/test_tree_guard.py',
                                                              'tests/fixtures/guard_durability.c',
                                                              'tests/fixtures/guard_mounts.py',
                                                              'tests/fixtures/no_openat2.py']},
 'test_private_mounts_excluded_from_anchored_descent': {'owner': 'Pinned Linux comparison '
                                                                 'descendant mount scope and '
                                                                 'fail-closed identity',
                                                        'primary': 'Linux integration',
                                                        'sources': ['linux/fpguard.c',
                                                                    'linux/fpmount.h',
                                                                    'linux/fpancestry.h',
                                                                    'src/pathguard.c',
                                                                    'src/comparetree.c',
                                                                    'src/wsl.c',
                                                                    'src/wslproto.h',
                                                                    'tests/test_tree_guard.py',
                                                                    'tests/fixtures/guard_durability.c',
                                                                    'tests/fixtures/guard_mounts.py',
                                                                    'tests/fixtures/no_openat2.py']},
 'test_private_mounts_excluded_with_openat2_fallback': {'owner': 'Pinned Linux comparison '
                                                                 'descendant mount scope and '
                                                                 'fail-closed identity',
                                                        'primary': 'Linux integration',
                                                        'sources': ['linux/fpguard.c',
                                                                    'linux/fpmount.h',
                                                                    'linux/fpancestry.h',
                                                                    'src/pathguard.c',
                                                                    'src/comparetree.c',
                                                                    'src/wsl.c',
                                                                    'src/wslproto.h',
                                                                    'tests/test_tree_guard.py',
                                                                    'tests/fixtures/guard_durability.c',
                                                                    'tests/fixtures/guard_mounts.py',
                                                                    'tests/fixtures/no_openat2.py']}}

class TreeGuard(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix='openpilot_guard_build_')
        cls.addClassCleanup(cls.build.cleanup)
        root = Path(__file__).resolve().parents[1]
        cls.fault_helper = str(Path(cls.build.name)/'guard-faults')
        source = Path(os.environ.get('FP_GUARD_CODE_ROOT', root))
        subprocess.run(['cc', '-std=c11', '-O2', '-I'+str(root/'src'), '-I'+str(root/'linux'),
                        str(source/'linux/fpguard.c'), str(root/'tests/fixtures/guard_durability.c'),
                        '-Wl,--wrap=fsync', '-Wl,--wrap=fopen', '-o', cls.fault_helper], check=True, capture_output=True)

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='openpilot_guard_')
        self.root = Path(self.tmp.name)
        self.procs = []

    def tearDown(self):
        for p in reversed(self.procs):
            p.stdin.close()
            try: p.wait(timeout=3)
            finally:
                if p.poll() is None: p.kill(); p.wait()
                p.stdout.close()
        self.tmp.cleanup()

    def read(self, p, n):
        data = b''
        while len(data) < n:
            self.assertTrue(select.select([p.stdout], [], [], 5)[0], 'helper deadline')
            v = os.read(p.stdout.fileno(), n-len(data))
            self.assertTrue(v, 'unexpected EOF'); data += v
        return data

    def guard(self, path, anchor=False, device=0, create=False, *, fault=None, fallback=False, mount_id_missing=False):
        env = None
        if fault is not None:
            env = dict(os.environ, FP_TEST_SYNC_LOG=str(self.root/'sync.log'),
                       FP_TEST_SYNC_NTH=str(fault[0]), FP_TEST_SYNC_ERRNO=str(fault[1]))
        helper = self.fault_helper if fault is not None or mount_id_missing else HELPER
        if mount_id_missing: env = {**os.environ, **(env or {}), 'FP_TEST_NO_MOUNT_ID':'1'}
        command = [helper, '--guard']
        if fallback:
            env = {**os.environ, **(env or {}), 'OPENPILOT_TEST_MOUNT_HELPER':helper}
            command = [sys.executable, str(ROOT/'tests/fixtures/no_openat2.py'), '--guard']
        p = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, env=env)
        self.procs.append(p)
        self.assertEqual(HEAD.unpack(self.read(p, 12)), (1, 16, 0)); self.read(p, 16)
        data = struct.pack('<QII', device, anchor, create)+os.fsencode(path)
        p.stdin.write(HEAD.pack(43, len(data), 1)+data); p.stdin.flush()
        self.assertEqual(HEAD.unpack(self.read(p, 12)), (44, 24, 1))
        dev, err, pid, fd, pad = struct.unpack('<QIIII', self.read(p, 24))
        self.assertEqual(pad, 0)
        if err: self.assertEqual((dev, pid, fd), (0, 0, 0), 'refusal must not publish a usable pin')
        return p, err, dev, f'/proc/{pid}/fd/{fd}'

    def sync(self, p):
        p.stdin.write(HEAD.pack(56, 0, 1)); p.stdin.flush()
        self.assertEqual(HEAD.unpack(self.read(p, 12)), (57, 4, 1))
        return struct.unpack('<i', self.read(p, 4))[0]

    def sync_log(self):
        self.assertTrue((self.root/'sync.log').is_file(), 'directory sync must precede the result or ACK')
        return [line.split(' ', 2) for line in (self.root/'sync.log').read_text().splitlines()]

    def test_creation_flushes_each_child_and_parent_before_result(self):
        _, err, dev, anchor = self.guard(self.root); self.assertEqual(err, 0)
        _, err, _, _ = self.guard(anchor+'/new/deep', True, dev, True, fault=(0, 0))
        self.assertEqual(err, 0)
        self.assertEqual([v[2] for v in self.sync_log()],
                         [str(self.root/'new'), str(self.root), str(self.root/'new/deep'), str(self.root/'new')])
        self.assertTrue(all(v[1]=='0' for v in self.sync_log()))

    def test_final_sync_uses_pinned_descriptor_after_name_replacement(self):
        directory = self.root/'selected'; directory.mkdir()
        p, err, _, anchor = self.guard(directory, fault=(0, 0)); self.assertEqual(err, 0)
        directory.rename(self.root/'held'); directory.symlink_to('/')
        self.assertEqual(self.sync(p), 0); self.assertEqual(self.sync(p), 0)
        self.assertEqual([v[2] for v in self.sync_log()], [str(self.root/'held')]*2)
        self.assertTrue(Path(anchor).is_dir()); self.assertIsNone(p.poll())

    def test_creation_flush_errors_attempt_both_and_fail_result(self):
        _, err, dev, anchor = self.guard(self.root); self.assertEqual(err, 0)
        for error in [errno.EIO, errno.EACCES, errno.EOPNOTSUPP]:
            for nth in [1, 2]:
                with self.subTest(error=error, nth=nth):
                    (self.root/'sync.log').unlink(missing_ok=True)
                    _, actual, _, _ = self.guard(anchor+'/new', True, dev, True, fault=(nth, error))
                    self.assertEqual(actual, error)
                    self.assertEqual(len(self.sync_log()), 2)
                    self.assertEqual(list((self.root/'new').iterdir()), [])

    def test_final_flush_error_ack_keeps_pin_and_sources(self):
        source = self.root/'source'; source.write_bytes(b'original')
        for error in [errno.EIO, errno.EACCES, errno.EOPNOTSUPP]:
            with self.subTest(error=error):
                p, actual, _, _ = self.guard(self.root, fault=(1, error)); self.assertEqual(actual, 0)
                self.assertEqual(self.sync(p), error)
                self.assertIsNone(p.poll()); self.assertEqual(source.read_bytes(), b'original')
                self.assertEqual(self.sync(p), 0)

    def test_interrupted_creation_and_final_flush_are_retried(self):
        _, err, dev, anchor = self.guard(self.root); self.assertEqual(err, 0)
        _, err, _, _ = self.guard(anchor+'/retry', True, dev, True, fault=(1, errno.EINTR))
        self.assertEqual(err, 0); self.assertEqual(len(self.sync_log()), 3)
        (self.root/'sync.log').unlink()
        p, err, _, _ = self.guard(self.root, fault=(1, errno.EINTR)); self.assertEqual(err, 0)
        self.assertEqual(self.sync(p), 0); self.assertEqual(len(self.sync_log()), 2)

    def test_invalid_sync_commands_never_flush(self):
        for packet in [HEAD.pack(56, 1, 1), HEAD.pack(56, 0, 2), HEAD.pack(55, 0, 1),
                       HEAD.pack(58, 1, 1), HEAD.pack(58, 0, 2)]:
            with self.subTest(packet=packet):
                p, err, _, _ = self.guard(self.root, fault=(0, 0)); self.assertEqual(err, 0)
                p.stdin.write(packet); p.stdin.flush()
                self.assertEqual(p.wait(timeout=3), 1)
                self.assertFalse((self.root/'sync.log').exists())

    def test_pinned_directory_survives_rename_and_link_replacement(self):
        d = self.root/'captured'; d.mkdir(); (d/'data').write_text('original')
        p, err, dev, anchor = self.guard(d); self.assertEqual(err, 0)
        d.rename(self.root/'moved'); d.symlink_to('/')
        self.assertEqual(Path(anchor, 'data').read_text(), 'original')
        self.assertEqual(os.stat(anchor).st_dev, dev)
        self.assertIsNone(p.poll())

    def test_descendants_never_follow_directory_links(self):
        (self.root/'link').symlink_to('/')
        _, err, dev, anchor = self.guard(self.root); self.assertEqual(err, 0)
        self.assertNotEqual(self.guard(anchor+'/link/etc', True, dev)[1], 0)
        self.assertNotEqual(self.guard(self.root/'link')[1], 0)

    def test_guarded_missing_parents_are_created_literally(self):
        _, err, dev, anchor = self.guard(self.root); self.assertEqual(err, 0)
        literal='a:b*$(literal);💀'
        _, err, _, child = self.guard(anchor+'/'+literal+'/deep', True, dev, True)
        self.assertEqual(err, 0); self.assertTrue((self.root/literal/'deep').is_dir())
        self.assertEqual(os.stat(child).st_ino, (self.root/literal/'deep').stat().st_ino)
        self.assertEqual(self.guard(self.root, create=True)[1], errno.EINVAL)

    def test_parent_traversal_and_device_changes_are_rejected(self):
        _, err, dev, anchor = self.guard(self.root); self.assertEqual(err, 0)
        for suffix in ('/../escape', '/./escape', '//escape'):
            self.assertEqual(self.guard(anchor+suffix, True, dev, True)[1], errno.EINVAL)
        self.assertEqual(self.guard(anchor, True, dev+1)[1], errno.EXDEV)
        self.assertFalse((self.root.parent/'escape').exists())

    def test_mount_boundary_stops_at_different_device(self):
        _, err, dev, anchor = self.guard('/'); self.assertEqual(err, 0)
        self.assertNotEqual(os.stat('/proc').st_dev, dev)
        self.assertEqual(self.guard(anchor+'/proc', True, dev)[1], errno.EXDEV)

    def test_permission_errors_remain_errors(self):
        self.assertNotEqual(os.getuid(), 0, 'run guard contracts as default unprivileged user')
        d=self.root/'denied'; d.mkdir(); d.chmod(0)
        try: self.assertEqual(self.guard(d)[1], errno.EACCES)
        finally: d.chmod(0o700)

    def test_eof_releases_live_anchor(self):
        p, err, _, anchor = self.guard(self.root); self.assertEqual(err, 0)
        p.stdin.close(); self.assertEqual(p.wait(timeout=3), 0)
        self.assertFalse(Path(anchor).exists())

    def test_mount_identity_unavailable_fails_closed(self):
        (self.root/'ordinary').mkdir()
        _, error, dev, anchor = self.guard(self.root, fallback=True, mount_id_missing=True)
        self.assertEqual(error, 0, 'explicit root needs no descendant mount lookup')
        self.assertTrue(Path(anchor).is_dir(), '/proc pin must remain usable')
        self.assertEqual(self.guard(anchor+'/ordinary', True, dev, fallback=True,
                                    mount_id_missing=True)[1], errno.ENOTSUP)
        self.assertEqual(list((self.root/'ordinary').iterdir()), [])

    def test_mount_identity_fallback_preserves_normal_children_and_link_refusal(self):
        (self.root/'ordinary').mkdir(); (self.root/'link').symlink_to(self.root/'ordinary')
        _, error, dev, anchor = self.guard(self.root, fallback=True)
        self.assertEqual(error, 0)
        self.assertEqual(self.guard(anchor+'/ordinary', True, dev, fallback=True)[1], 0)
        self.assertNotEqual(self.guard(anchor+'/link', True, dev, fallback=True)[1], 0)

    def private_mounts(self, fallback):
        result = subprocess.run(['unshare', '--user', '--map-root-user', '--mount', '--propagation', 'private',
            sys.executable, str(ROOT/'tests/fixtures/guard_mounts.py'), HELPER,
            os.readlink('/proc/self/ns/mnt'), str(int(fallback))], capture_output=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stderr.decode(errors='replace'))
        report = json.loads(result.stdout)
        self.assertNotEqual(report['namespace'], report['parent_namespace'])
        self.assertTrue(report['private_before_mounts']); self.assertTrue(report['sentinels_unchanged'])
        self.assertEqual(report['fallback'], fallback); self.assertEqual(len(report['cases']), 12)
        self.assertEqual(sum(x['error']==errno.EXDEV for x in report['cases']), 2)
        evidence = os.environ.get('OPENPILOT_N07_EVIDENCE')
        if evidence: Path(evidence, 'mounts-fallback-'+str(int(fallback))+'.json').write_text(json.dumps(report, indent=2)+'\n')

    def test_private_mounts_excluded_from_anchored_descent(self):
        self.private_mounts(False)

    def test_private_mounts_excluded_with_openat2_fallback(self):
        self.private_mounts(True)

    def regular_copy(self, change):
        source=self.root/'selected'; source.write_bytes(b'chosen')
        dest=self.root/'destination'; dest.mkdir()
        p=subprocess.Popen([HELPER, '--jobs'], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        self.procs.append(p)
        self.assertEqual(HEAD.unpack(self.read(p,12)), (1,16,0)); self.read(p,16)
        body=struct.pack('<IIII',9,0,1,0)
        for name in (dest,source):
            raw=os.fsencode(name); body+=struct.pack('<II',len(raw),0)+raw
        p.stdin.write(HEAD.pack(10,len(body),1)+body); p.stdin.flush(); continued=False
        while True:
            typ,n,ident=HEAD.unpack(self.read(p,12)); self.assertEqual(ident,1); data=self.read(p,n)
            if typ==12: continue
            self.assertEqual(typ,11)
            values=struct.unpack('<QQIIIIIiII',data[:48]);state=values[2]
            if state==2 and not continued:
                if change:
                    source.unlink(); source.mkdir(); (source/'unselected').write_text('secret')
                p.stdin.write(HEAD.pack(14,0,1));p.stdin.flush();continued=True
            if state>=3: break
        self.assertEqual(p.wait(timeout=3),0)
        return dest,values

    def test_regular_copy_copies_only_the_selected_file(self):
        dest,v=self.regular_copy(False);self.assertEqual(v[5],0)
        self.assertEqual((dest/'selected').read_bytes(),b'chosen')
        self.assertEqual(v[3:5],(1,1))

    def test_selected_file_replaced_by_folder_never_recurses(self):
        dest,v=self.regular_copy(True);self.assertEqual(v[5],1)
        self.assertEqual(list(dest.iterdir()),[])

if __name__=='__main__': unittest.main(verbosity=2)
