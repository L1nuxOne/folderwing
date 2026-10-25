"""Real mount boundaries in an owned user/mount namespace; no behavior skips.
Run: python3 tests/test_mount_jobs.py /absolute/path/to/folderwing-wsl
The launcher isolates mounts before making any fixtures or starting helpers.
"""
import errno
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

import test_wsl_jobs as jobs
import test_cross_distro as cross

HELPER = str(Path(sys.argv.pop(1)).resolve()) if __name__ == '__main__' and len(sys.argv) > 1 and not sys.argv[1].startswith('-') else jobs.HELPER
PARENT_NAMESPACE = None
if '--private-mount-namespace' in sys.argv:
    index = sys.argv.index('--private-mount-namespace')
    PARENT_NAMESPACE = sys.argv[index + 1]
    del sys.argv[index:index + 2]


class MountJobs(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not PARENT_NAMESPACE or os.readlink('/proc/self/ns/mnt') == PARENT_NAMESPACE:
            raise RuntimeError('run this module through its private namespace launcher')

    def setUp(self):
        self.native = jobs.NativeJobs(methodName='runTest'); self.native.setUp()
        self.stream = cross.CrossDistro(methodName='runTest'); self.stream.setUp()
        self.root, self.src, self.dst = self.native.root, self.native.src, self.native.dst
        self.mounts = []
        jobs.HELPER = cross.HELPER = HELPER

    def tearDown(self):
        # Stop helper FDs before unmounting, then allow normal temp-tree cleanup.
        for p in self.stream.children:
            if p.poll() is None: p.kill(); p.wait()
        for target in reversed(self.mounts):
            subprocess.run(['umount', '--', str(target)], check=True, capture_output=True)
        self.stream.tearDown(); self.native.tearDown()
        jobs.HELPER = cross.HELPER = HELPER

    def mount(self, source, target, kind='bind'):
        command = ['mount', '--bind', str(source), str(target)] if kind == 'bind' else ['mount', '-t', 'tmpfs', '-o', 'size=1m', 'none', str(target)]
        subprocess.run(command, check=True, capture_output=True)
        self.mounts.append(target)

    def fixture(self, kind, name='tree'):
        tree = self.src/name; tree.mkdir(); (tree/'ordinary').write_bytes(b'ordinary')
        outside = self.root/(name+'-outside'); outside.mkdir()
        target = tree/'mounted'
        if kind == 'file':
            sentinel = outside/'sentinel'; sentinel.write_bytes(b'outside sentinel')
            target.touch(); self.mount(sentinel, target)
            self.assertEqual(target.stat().st_dev, tree.stat().st_dev)
        else:
            target.mkdir(); self.mount(outside, target, kind)
            sentinel = target/'sentinel'; sentinel.write_bytes(b'outside sentinel')
            self.assertEqual(target.stat().st_dev == tree.stat().st_dev, kind == 'bind')
        before = sentinel.stat()
        return tree, target, sentinel, (before.st_ino, before.st_mode, before.st_mtime_ns)

    def unchanged(self, sentinel, identity):
        self.assertEqual(sentinel.read_bytes(), b'outside sentinel')
        now = sentinel.stat(); self.assertEqual((now.st_ino, now.st_mode, now.st_mtime_ns), identity)

    def boundary(self, result, suffix=b'/mounted'):
        self.assertEqual(result['state'], 3)
        self.assertEqual(result['errors'], 1, result)
        self.assertEqual(result['skipped'], 0)
        self.assertTrue(any(event[1] == errno.EXDEV and event[-1].endswith(suffix) for event in result['events']), result)

    def test_copy_and_scan_exclude_real_and_bind_mounted_children(self):
        for kind in ('tmpfs', 'bind', 'file'):
            with self.subTest(kind=kind):
                tree, target, sentinel, identity = self.fixture(kind, kind)
                result = self.native.job(0, [tree], self.dst)
                self.boundary(result); self.unchanged(sentinel, identity)
                copied = self.dst/tree.name
                self.assertEqual((copied/'ordinary').read_bytes(), b'ordinary')
                self.assertFalse((copied/'mounted').exists())
                self.assertEqual(result['total'], len(b'ordinary'))
                self.assertTrue(target.exists())

    def test_delete_preserves_mounts_and_outside_sentinels(self):
        for kind in ('tmpfs', 'bind', 'file'):
            with self.subTest(kind=kind):
                tree, target, sentinel, identity = self.fixture(kind, kind)
                result = self.native.job(2, [tree])
                self.boundary(result); self.unchanged(sentinel, identity)
                self.assertTrue(tree.exists()); self.assertTrue(target.exists())
                self.assertFalse((tree/'ordinary').exists())

    def test_merge_move_retains_the_mount_but_moves_ordinary_siblings(self):
        for kind in ('tmpfs', 'bind', 'file'):
            with self.subTest(kind=kind):
                tree, _, sentinel, identity = self.fixture(kind, kind)
                (self.dst/tree.name).mkdir()
                result = self.native.job(1, [tree], self.dst, policy=2)
                self.boundary(result); self.unchanged(sentinel, identity)
                self.assertTrue(tree.exists()); self.assertFalse((tree/'ordinary').exists())
                self.assertEqual((self.dst/tree.name/'ordinary').read_bytes(), b'ordinary')

    def test_cross_volume_move_retains_the_mounted_source(self):
        with tempfile.TemporaryDirectory(prefix='openpilot_mount_move_', dir='/dev/shm') as dest:
            for kind in ('tmpfs', 'bind'):
                with self.subTest(kind=kind):
                    tree, _, sentinel, identity = self.fixture(kind, kind)
                    self.assertNotEqual(tree.stat().st_dev, os.stat(dest).st_dev)
                    result = self.native.job(1, [tree], dest)
                    self.boundary(result); self.unchanged(sentinel, identity)
                    self.assertTrue(tree.exists()); self.assertFalse((tree/'ordinary').exists())
                    self.assertEqual((Path(dest)/tree.name/'ordinary').read_bytes(), b'ordinary')

    def test_selected_mountpoints_are_refused_before_any_transfer(self):
        for kind in ('tmpfs', 'bind', 'file'):
            with self.subTest(kind=kind):
                _, target, sentinel, identity = self.fixture(kind, kind)
                for op in (0, 1, 2):
                    result = self.native.job(op, [target], self.dst)
                    self.boundary(result, b''); self.assertEqual(list(self.dst.iterdir()), [])
                for op in (0, 1):
                    result = self.stream.transfer(target, op=op, destination=self.dst)
                    self.boundary(result, b''); self.assertEqual(list(self.dst.iterdir()), [])
                self.unchanged(sentinel, identity)

    def test_destination_merges_cannot_enter_mounted_folders(self):
        for kind in ('tmpfs', 'bind'):
            for op in (0, 1):
                with self.subTest(kind=kind, op=op):
                    name = kind+str(op)
                    tree = self.src/name; tree.mkdir(); (tree/'ordinary').write_bytes(b'ordinary')
                    child = tree/'mounted'; child.mkdir(); (child/'new').write_bytes(b'new')
                    dest = self.dst/name; dest.mkdir(); target = dest/'mounted'; target.mkdir()
                    outside = self.root/(name+'-outside'); outside.mkdir(); self.mount(outside, target, kind)
                    sentinel = target/'sentinel'; sentinel.write_bytes(b'outside sentinel'); st = sentinel.stat()
                    result = self.native.job(op, [tree], self.dst, policy=2)
                    self.boundary(result); self.unchanged(sentinel, (st.st_ino, st.st_mode, st.st_mtime_ns))
                    self.assertEqual(list(target.iterdir()), [sentinel]); self.assertTrue((child/'new').exists())
                    self.assertEqual((dest/'ordinary').read_bytes(), b'ordinary')

    def test_export_copy_and_move_exclude_mounted_children(self):
        for kind in ('tmpfs', 'bind', 'file'):
            for op in (0, 1):
                with self.subTest(kind=kind, op=op):
                    tree, _, sentinel, identity = self.fixture(kind, kind+str(op))
                    result = self.stream.transfer(tree, op=op, destination=self.dst)
                    self.boundary(result); self.unchanged(sentinel, identity)
                    self.assertEqual(result['streamed'], len(b'ordinary'))
                    self.assertEqual((self.dst/tree.name/'ordinary').read_bytes(), b'ordinary')
                    self.assertFalse((self.dst/tree.name/'mounted').exists()); self.assertTrue(tree.exists())
                    self.assertEqual((tree/'ordinary').exists(), op == 0)

    def test_import_merges_cannot_write_into_mounted_folders(self):
        for kind in ('tmpfs', 'bind'):
            tree = self.src/kind; tree.mkdir(); (tree/'new').write_bytes(b'new')
            target = self.dst/kind; target.mkdir(); outside = self.root/(kind+'-outside'); outside.mkdir()
            self.mount(outside, target, kind); sentinel = target/'sentinel'; sentinel.write_bytes(b'outside sentinel')
            before = sentinel.stat()
            result = self.stream.transfer(tree, op=1, policy=2, destination=self.dst)
            self.boundary(result, b''); self.unchanged(sentinel, (before.st_ino, before.st_mode, before.st_mtime_ns))
            self.assertEqual(list(target.iterdir()), [sentinel]); self.assertTrue((tree/'new').exists())

    def test_mount_added_after_scan_is_rechecked_before_mutation(self):
        for op in (0, 1, 2):
            with self.subTest(op=op):
                name = 'late'+str(op)
                tree = self.src/name; tree.mkdir(); target = tree/'mounted'; target.mkdir()
                if op == 1: (self.dst/name).mkdir() # Exercise the recursive merge, not a rename.
                outside = self.root/(name+'-outside'); outside.mkdir(); sentinel = outside/'sentinel'; sentinel.write_bytes(b'outside sentinel')
                before = sentinel.stat(); mounted = False
                def mutate(typ, body):
                    nonlocal mounted
                    if typ == 11 and struct.unpack_from('<I', body, 16)[0] == 2 and not mounted:
                        self.mount(outside, target); mounted = True
                result = self.native.job(op, [tree], self.dst, policy=2, mutate=mutate)
                self.boundary(result); self.unchanged(sentinel, (before.st_ino, before.st_mode, before.st_mtime_ns))
                self.assertTrue(tree.exists()); self.assertTrue(mounted)

    def test_explicit_endpoints_inside_a_mount_still_work(self):
        mounted = self.root/'volume'; mounted.mkdir(); self.mount(None, mounted, 'tmpfs')
        inside = mounted/'chosen'; inside.write_bytes(b'chosen')
        result = self.native.job(0, [inside], self.dst)
        self.assertEqual(result['errors'], 0); self.assertEqual((self.dst/'chosen').read_bytes(), b'chosen')
        source = self.src/'file'; source.write_bytes(b'file')
        result = self.native.job(0, [source], mounted)
        self.assertEqual(result['errors'], 0); self.assertEqual((mounted/'file').read_bytes(), b'file')
        result = self.stream.transfer(source, destination=mounted)
        self.assertEqual(result['errors'], 0); self.assertEqual((mounted/'file (2)').read_bytes(), b'file')

    def fallback(self):
        wrapper = str(Path(__file__).parent/'fixtures/no_openat2.py')
        self.native.env['OPENPILOT_TEST_MOUNT_HELPER'] = HELPER
        old = os.environ.get('OPENPILOT_TEST_MOUNT_HELPER')
        os.environ['OPENPILOT_TEST_MOUNT_HELPER'] = HELPER
        self.addCleanup(lambda: os.environ.pop('OPENPILOT_TEST_MOUNT_HELPER', None) if old is None else os.environ.__setitem__('OPENPILOT_TEST_MOUNT_HELPER', old))
        jobs.HELPER = cross.HELPER = wrapper

    def test_old_kernel_fallback_rejects_same_device_bind_mounts(self):
        self.fallback()
        tree, _, sentinel, identity = self.fixture('bind')
        result = self.native.job(0, [tree], self.dst)
        self.boundary(result); self.unchanged(sentinel, identity)
        self.assertFalse((self.dst/'tree/mounted').exists())
        result = self.stream.transfer(tree, op=1, policy=2, destination=self.dst)
        self.boundary(result); self.unchanged(sentinel, identity)
        result = self.native.job(2, [tree])
        self.boundary(result); self.unchanged(sentinel, identity)

    def test_old_kernel_without_mount_identity_fails_closed(self):
        self.fallback()
        source = self.src/'chosen'; source.write_bytes(b'chosen')
        self.mount(None, Path('/proc'), 'tmpfs')
        result = self.native.job(2, [source])
        self.assertEqual(result['errors'], 1); self.assertEqual(result['events'][0][1], errno.ENOTSUP)
        self.assertEqual(source.read_bytes(), b'chosen')

    def test_modern_kernel_checks_mounts_without_procfs(self):
        tree, _, sentinel, identity = self.fixture('bind')
        self.mount(None, Path('/proc'), 'tmpfs')
        result = self.native.job(0, [tree], self.dst)
        self.boundary(result); self.unchanged(sentinel, identity)
        self.assertEqual((self.dst/'tree/ordinary').read_bytes(), b'ordinary')
        self.assertFalse((self.dst/'tree/mounted').exists())
        result = self.stream.transfer(tree, op=1, policy=2, destination=self.dst)
        self.boundary(result); self.unchanged(sentinel, identity)


if __name__ == '__main__':
    if PARENT_NAMESPACE is None:
        command = ['unshare', '--user', '--map-root-user', '--mount', '--propagation', 'private',
                   sys.executable, str(Path(__file__).resolve()), HELPER, '--private-mount-namespace',
                   os.readlink('/proc/self/ns/mnt'), *sys.argv[1:]]
        raise SystemExit(subprocess.call(command))
    unittest.main(verbosity=2)
