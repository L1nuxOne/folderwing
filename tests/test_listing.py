"""Linux listing completeness: real permissions and test-only statx boundaries."""
import errno
import os
from pathlib import Path
import select
import stat
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
HELPER = sys.argv.pop(1) if __name__ == '__main__' and len(sys.argv) > 1 and not sys.argv[1].startswith('-') else str(ROOT/'build/release/folderwing-wsl')
HEAD = struct.Struct('<III')
ENTRY = struct.Struct('<QqqIIIHHI4x')

TEST_CONTRACTS = {'test_controlled_nonbenign_lookup_errors_fail': {'owner': 'Linux listing completeness and '
                                                           'non-benign lookup error propagation',
                                                  'primary': 'Linux integration',
                                                  'sources': ['linux/fpwsl.c',
                                                              'src/wslproto.h',
                                                              'src/wsl.c',
                                                              'src/compare.c',
                                                              'src/comparetree.c',
                                                              'app/foldercompare.c',
                                                              'tests/test_listing.py',
                                                              'tests/fixtures/listing_errors.c']},
 'test_deletion_after_enumeration_is_benign': {'owner': 'Linux listing completeness and non-benign '
                                                        'lookup error propagation',
                                               'primary': 'Linux integration',
                                               'sources': ['linux/fpwsl.c',
                                                           'src/wslproto.h',
                                                           'src/wsl.c',
                                                           'src/compare.c',
                                                           'src/comparetree.c',
                                                           'app/foldercompare.c',
                                                           'tests/test_listing.py',
                                                           'tests/fixtures/listing_errors.c']},
 'test_error_does_not_poison_next_listing': {'owner': 'Linux listing completeness and non-benign '
                                                      'lookup error propagation',
                                             'primary': 'Linux integration',
                                             'sources': ['linux/fpwsl.c',
                                                         'src/wslproto.h',
                                                         'src/wsl.c',
                                                         'src/compare.c',
                                                         'src/comparetree.c',
                                                         'app/foldercompare.c',
                                                         'tests/test_listing.py',
                                                         'tests/fixtures/listing_errors.c']},
 'test_missing_directory_remains_an_error': {'owner': 'Linux listing completeness and non-benign '
                                                      'lookup error propagation',
                                             'primary': 'Linux integration',
                                             'sources': ['linux/fpwsl.c',
                                                         'src/wslproto.h',
                                                         'src/wsl.c',
                                                         'src/compare.c',
                                                         'src/comparetree.c',
                                                         'app/foldercompare.c',
                                                         'tests/test_listing.py',
                                                         'tests/fixtures/listing_errors.c']},
 'test_multiple_entry_packets_have_exact_count': {'owner': 'Linux listing completeness and '
                                                           'non-benign lookup error propagation',
                                                  'primary': 'Linux integration',
                                                  'sources': ['linux/fpwsl.c',
                                                              'src/wslproto.h',
                                                              'src/wsl.c',
                                                              'src/compare.c',
                                                              'src/comparetree.c',
                                                              'app/foldercompare.c',
                                                              'tests/test_listing.py',
                                                              'tests/fixtures/listing_errors.c']},
 'test_normal_types_names_and_broken_links': {'owner': 'Linux listing completeness and non-benign '
                                                       'lookup error propagation',
                                              'primary': 'Linux integration',
                                              'sources': ['linux/fpwsl.c',
                                                          'src/wslproto.h',
                                                          'src/wsl.c',
                                                          'src/compare.c',
                                                          'src/comparetree.c',
                                                          'app/foldercompare.c',
                                                          'tests/test_listing.py',
                                                          'tests/fixtures/listing_errors.c']},
 'test_readable_nonsearchable_directory_fails_for_real_user': {'owner': 'Linux listing '
                                                                        'completeness and '
                                                                        'non-benign lookup error '
                                                                        'propagation',
                                                               'primary': 'Linux integration',
                                                               'sources': ['linux/fpwsl.c',
                                                                           'src/wslproto.h',
                                                                           'src/wsl.c',
                                                                           'src/compare.c',
                                                                           'src/comparetree.c',
                                                                           'app/foldercompare.c',
                                                                           'tests/test_listing.py',
                                                                           'tests/fixtures/listing_errors.c']}}

class Listing(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix='openpilot_listing_build_')
        cls.addClassCleanup(cls.build.cleanup)
        source = Path(os.environ.get('FP_LIST_CODE_ROOT', ROOT))
        cls.fault_helper = str(Path(cls.build.name)/'listing-faults')
        subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra',
                        '-Wno-missing-field-initializers', '-I'+str(ROOT/'src'),
                        str(source/'linux/fpwsl.c'), str(ROOT/'tests/fixtures/listing_errors.c'),
                        '-Wl,--wrap=statx', '-o', cls.fault_helper], check=True, capture_output=True)

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='openpilot_listing_')
        self.root = Path(self.tmp.name)

    def tearDown(self):
        for path in self.root.iterdir():
            if path.is_dir(): path.chmod(0o700)
        self.tmp.cleanup()

    def read(self, p, count):
        data = b''
        while len(data) < count:
            self.assertTrue(select.select([p.stdout], [], [], 5)[0], 'helper reply deadline')
            block = os.read(p.stdout.fileno(), count-len(data))
            self.assertTrue(block, 'unexpected helper EOF'); data += block
        return data

    def listing(self, paths, action=None):
        env = dict(os.environ)
        if action is not None: env['FP_LIST_TEST_ACTION'] = str(action)
        p = subprocess.Popen([self.fault_helper if action is not None else HELPER],
                             stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE, env=env)
        try:
            self.assertEqual(HEAD.unpack(self.read(p, 12)), (1, 16, 0))
            _, _, uid, _ = struct.unpack('<IIII', self.read(p, 16))
            self.assertEqual(uid, os.getuid())
            results = []
            for ident, path in enumerate(paths, 1):
                raw = os.fsencode(path)
                p.stdin.write(HEAD.pack(2, len(raw), ident)+raw); p.stdin.flush()
                entries = {}; packets = 0
                while True:
                    kind, size, actual_id = HEAD.unpack(self.read(p, 12))
                    self.assertEqual(actual_id, ident); self.assertLessEqual(size, 65536)
                    body = self.read(p, size)
                    if kind == 3:
                        packets += 1; at = 0
                        while at < size:
                            values = ENTRY.unpack_from(body, at)
                            n, link = values[6:8]; begin = at+ENTRY.size
                            name = body[begin:begin+n]
                            self.assertNotIn(name, entries)
                            entries[name] = (values, body[begin+n:begin+n+link])
                            at += (ENTRY.size+n+link+7)&~7
                        self.assertEqual(at, size)
                    elif kind == 5:
                        error, count = struct.unpack('<iI', body)
                        self.assertEqual(count, len(entries))
                        results.append(dict(error=error, entries=entries, packets=packets)); break
                    else: self.assertEqual(kind, 4)  # OWNERS
            p.stdin.close(); self.assertEqual(p.wait(timeout=5), 0, p.stderr.read())
            return results
        finally:
            if p.poll() is None: p.kill(); p.wait()
            if not p.stdin.closed: p.stdin.close()
            p.stdout.close(); p.stderr.close()

    def test_normal_types_names_and_broken_links(self):
        (self.root/'regular').write_bytes(b'bytes'); (self.root/'directory').mkdir()
        (self.root/'broken').symlink_to('missing'); (self.root/'link').symlink_to('regular')
        (self.root/'literal :💀').write_bytes(b''); os.mkfifo(self.root/'fifo')
        result, = self.listing([self.root]); self.assertEqual(result['error'], 0)
        self.assertEqual(set(result['entries']), {os.fsencode(x.name) for x in self.root.iterdir()})
        self.assertEqual(result['entries'][b'regular'][0][0], 5)
        self.assertTrue(stat.S_ISDIR(result['entries'][b'directory'][0][3]))
        self.assertTrue(stat.S_ISFIFO(result['entries'][b'fifo'][0][3]))
        self.assertEqual(result['entries'][b'broken'][1], b'missing')
        self.assertEqual(result['entries'][b'broken'][0][8]&2, 2)

    def test_multiple_entry_packets_have_exact_count(self):
        names = {('entry-%04d-'%i+'x'*100).encode() for i in range(1100)}
        for name in names: (self.root/os.fsdecode(name)).touch()
        result, = self.listing([self.root]); self.assertEqual(result['error'], 0)
        self.assertEqual(set(result['entries']), names); self.assertGreater(result['packets'], 1)

    def test_readable_nonsearchable_directory_fails_for_real_user(self):
        self.assertNotEqual(os.geteuid(), 0, 'permission gate requires an actual unprivileged user')
        denied = self.root/'denied'; denied.mkdir(); (denied/'hidden').write_bytes(b'retain')
        denied.chmod(0o400)
        self.assertEqual(os.listdir(denied), ['hidden'])
        result, = self.listing([denied]); self.assertEqual(result['error'], errno.EACCES)
        self.assertEqual(result['entries'], {})
        denied.chmod(0o700); self.assertEqual((denied/'hidden').read_bytes(), b'retain')

    def test_controlled_nonbenign_lookup_errors_fail(self):
        (self.root/'keep').write_bytes(b'keep'); (self.root/'fault').write_bytes(b'retain')
        for error in [errno.EACCES, errno.EIO, errno.ENOTSUP, errno.ENOSYS, errno.ENOTDIR, errno.ELOOP, errno.EINTR]:
            with self.subTest(error=error):
                result, = self.listing([self.root], error)
                self.assertEqual(result['error'], error)
                self.assertNotIn(b'fault', result['entries'])
                self.assertEqual((self.root/'fault').read_bytes(), b'retain')
                self.assertEqual((self.root/'keep').read_bytes(), b'keep')

    def test_deletion_after_enumeration_is_benign(self):
        (self.root/'keep').write_bytes(b'keep'); (self.root/'fault').write_bytes(b'owned')
        result, = self.listing([self.root], 'delete')
        self.assertEqual(result['error'], 0); self.assertEqual(set(result['entries']), {b'keep'})
        self.assertFalse((self.root/'fault').exists()); self.assertEqual((self.root/'keep').read_bytes(), b'keep')

    def test_error_does_not_poison_next_listing(self):
        failed = self.root/'failed'; good = self.root/'good'; failed.mkdir(); good.mkdir()
        (failed/'fault').touch(); (good/'exact').write_bytes(b'valid')
        first, second = self.listing([failed, good], errno.EIO)
        self.assertEqual(first['error'], errno.EIO); self.assertEqual(second['error'], 0)
        self.assertEqual(set(second['entries']), {b'exact'})

    def test_missing_directory_remains_an_error(self):
        result, = self.listing([self.root/'absent'])
        self.assertEqual(result['error'], errno.ENOENT); self.assertEqual(result['entries'], {})

if __name__ == '__main__': unittest.main()
