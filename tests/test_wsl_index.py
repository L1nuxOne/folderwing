"""Optional per-user index: truthful coverage, raw paths and native lifecycle."""
import errno
import hashlib
import os
from pathlib import Path
import select
import sqlite3
import struct
import subprocess
import sys
import tempfile
import time
import unittest

HELPER = sys.argv.pop(1)
HEAD = struct.Struct('<III')
STATUS = struct.Struct('<IIIIQQQQQII')
ENTRY = struct.Struct('<QqII')
END = struct.Struct('<IIII')


class Session:
    def __init__(self, root, cache, action=0):
        self.scope = os.fsencode(root)
        self.p = subprocess.Popen([HELPER, '--index'], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  env=dict(os.environ, XDG_CACHE_HOME=str(cache)), bufsize=0)
        self.ident = 1
        kind, ident, data = self.receive()
        assert kind == 53 and ident == 0 and len(data) == 16
        self.uid = struct.unpack('<IIII', data)[2]
        self.send(48, struct.pack('<II', action, len(self.scope)) + self.scope)
        self.initial = self.status_reply()

    def read(self, count):
        data = b''
        until = time.monotonic() + 10
        while len(data) < count:
            if not select.select([self.p.stdout], [], [], max(0, until - time.monotonic()))[0]:
                raise AssertionError('index helper response timed out')
            chunk = os.read(self.p.stdout.fileno(), count - len(data))
            if not chunk:
                raise AssertionError('index helper closed its response')
            data += chunk
        return data

    def send(self, kind, data=b''):
        self.p.stdin.write(HEAD.pack(kind, len(data), self.ident) + data)

    def receive(self):
        kind, size, ident = HEAD.unpack(self.read(HEAD.size))
        assert size <= STATUS.size + 16384
        return kind, ident, self.read(size)

    def decode(self, data):
        fields = STATUS.unpack_from(data)
        keys = ['state', 'error', 'uid', 'enabled', 'entries', 'scanned', 'errors', 'skipped', 'updated', 'root_bytes', 'pad']
        result = dict(zip(keys, fields))
        assert result['uid'] == self.uid == os.getuid() and result['pad'] == 0
        result['root'] = data[STATUS.size:]
        assert len(result['root']) == result['root_bytes']
        return result

    def status_reply(self):
        kind, ident, data = self.receive()
        assert kind == 49 and ident == self.ident
        return self.decode(data)

    def control(self, action=0):
        self.ident += 1
        self.send(50, struct.pack('<I', action))
        return self.status_reply()

    def wait_state(self, state=2):
        until = time.monotonic() + 10
        while True:
            result = self.control()
            if result['state'] == state:
                return result
            if time.monotonic() >= until:
                raise AssertionError(f'index did not reach state {state}: {result}')
            time.sleep(.03)

    def query(self, glob=b'*needle*', scope=None, cancel=False):
        scope = self.scope if scope is None else os.fsencode(scope)
        self.ident += 1
        self.send(51, struct.pack('<II', len(scope), len(glob)) + scope + glob)
        if cancel:
            self.send(52)
        rows, status = [], None
        while True:
            kind, ident, data = self.receive()
            assert ident == self.ident
            if kind == 49:
                status = self.decode(data)
            elif kind == 47:
                assert status is not None
                return rows, status, END.unpack(data)
            else:
                assert kind == 46 and status is None
                size, mtime, attrs, length = ENTRY.unpack_from(data)
                raw = data[ENTRY.size:]
                assert length == len(raw) and raw and not raw.startswith(b'/')
                rows.append((raw, size, mtime, attrs))

    def close(self, kill=False):
        if self.p.poll() is None:
            if kill:
                self.p.kill()
            self.p.stdin.close()
            self.p.wait(timeout=5)
        else:
            self.p.stdin.close()
        self.p.stdout.close()


class WslIndex(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='openpilot-index-contract-')
        self.base = Path(self.tmp.name)
        self.root = self.base / 'files'
        self.root.mkdir()
        self.cache = self.base / 'cache'
        self.sessions = []

    def tearDown(self):
        for s in reversed(self.sessions):
            s.close()
        self.tmp.cleanup()

    def session(self, action=1, root=None):
        s = Session(root or self.root, self.cache, action)
        self.sessions.append(s)
        return s

    def database(self, root=None):
        name = hashlib.sha256(os.fsencode(root or self.root)).hexdigest() + '.db'
        return self.cache / 'openpilot-search-v1' / name

    def test_disabled_probe_creates_no_cache_or_watch_service(self):
        s = self.session(0)
        self.assertEqual(s.initial['state'], 0)
        self.assertEqual(s.initial['enabled'], 0)
        self.assertFalse(self.cache.exists())
        self.assertEqual(s.p.wait(timeout=5), 0)

    def test_raw_paths_candidates_and_fresh_metadata(self):
        self.assertNotEqual(os.getuid(), 0, 'contracts require the normal Linux user')
        for name in [b'needle:a\\b', b'raw\xff', b'raw\xfe', b'raw\\xff', 'Ωmega'.encode(), b'other']:
            fd = os.open(os.fsencode(self.root) + b'/' + name, os.O_CREAT | os.O_WRONLY, 0o600)
            os.write(fd, b'abc'); os.close(fd)
        s = self.session(); self.assertNotEqual(s.initial['state'], 2)
        self.assertEqual(s.wait_state()['entries'], 6)
        rows, status, end = s.query()
        names = {row[0] for row in rows}
        self.assertEqual(names, {b'needle:a\\b', b'raw\xff', b'raw\xfe', 'Ωmega'.encode()})
        self.assertTrue(all(row[1] == 3 and row[2] > 0 and row[3] == 0x80 for row in rows))
        self.assertEqual(status['state'], 2); self.assertEqual(end[0], 0)
        self.assertEqual(self.database().stat().st_mode & 0o777, 0o600)

    def test_subtree_scope_and_enabled_ancestor_on_reopen(self):
        child = self.root / 'child'; child.mkdir()
        (child / 'needle.txt').write_text('child'); (self.root / 'needle.txt').write_text('parent')
        s = self.session(); s.wait_state()
        rows, _, end = s.query(scope=child)
        self.assertEqual([(r[0], r[1]) for r in rows], [(b'needle.txt', 5)])
        self.assertEqual(end[0], 0); s.close()
        second = self.session(0, child)
        self.assertEqual(second.initial['root'], os.fsencode(self.root))
        self.assertNotEqual(second.initial['state'], 2)
        second.wait_state()
        self.assertEqual([r[0] for r in second.query()[0]], [b'needle.txt'])

    def test_unwatched_ancestor_moves_invalidate_and_recover_the_selected_path(self):
        parent = self.root / 'parent'; parent.mkdir()
        selected = parent / 'selected'; selected.mkdir(); (selected / 'needle').touch()
        s = self.session(1, selected); s.wait_state()
        moved = self.root / 'moved'; parent.rename(moved)
        try:
            self.assertNotEqual(s.control()['state'], 2)
            self.assertEqual(s.query()[0], [])
            self.assertEqual(s.wait_state(5)['error'], errno.ENOENT)
        finally:
            moved.rename(parent)
        s.wait_state()
        self.assertEqual([r[0] for r in s.query()[0]], [b'needle'])

    def test_changes_pause_resume_and_remove_preserve_user_files(self):
        old = self.root / 'old-needle'; old.write_text('keep')
        s = self.session(); ready = s.wait_state()
        old.rename(self.root / 'new-needle')
        (self.root / 'created-needle').write_text('new')
        s.wait_state()
        rows, _, _ = s.query()
        self.assertEqual({r[0] for r in rows}, {b'new-needle', b'created-needle'})
        self.assertEqual(s.control(2)['state'], 4)
        (self.root / 'paused-needle').write_text('paused')
        self.assertEqual(s.query()[2][0], errno.EAGAIN)
        s.close()
        paused = self.session(0); self.assertEqual(paused.initial['state'], 4)
        self.assertEqual(paused.control(3)['enabled'], 1); paused.wait_state()
        self.assertEqual(len(paused.query()[0]), 3)
        self.assertEqual(paused.control(5)['state'], 0)
        self.assertFalse(self.database().exists())
        self.assertEqual((self.root / 'new-needle').read_text(), 'keep')
        self.assertEqual(self.session(0).initial['state'], 0)

    def test_permission_loss_never_serves_committed_inaccessible_names(self):
        child = self.root / 'child'; child.mkdir(); (child / 'secret-needle').write_text('keep')
        s = self.session(); s.wait_state(); child.chmod(0)
        try:
            failed = s.wait_state(5)
            self.assertEqual(failed['error'], errno.EACCES)
            self.assertEqual(s.query()[0], [])
            # A failed replacement leaves the previous committed database intact.
            with sqlite3.connect(self.database()) as c:
                self.assertEqual(c.execute('SELECT count(*) FROM entries').fetchone()[0], 2)
        finally:
            child.chmod(0o700)
        s.wait_state()
        self.assertEqual([r[0] for r in s.query()[0]], [b'child/secret-needle'])

    def test_links_special_nodes_and_depth_bound_never_claim_complete(self):
        outside = self.base / 'outside'; outside.mkdir(); (outside / 'secret-needle').write_text('keep')
        (self.root / 'link-needle').symlink_to(outside, target_is_directory=True)
        os.mkfifo(self.root / 'fifo-needle')
        s = self.session(); s.wait_state()
        rows = {r[0]: r for r in s.query()[0]}
        self.assertEqual(set(rows), {b'link-needle', b'fifo-needle'})
        self.assertTrue(all(r[3] == 0x400 for r in rows.values()))
        s.close()
        here = self.root
        for _ in range(130):
            here = here / 'd'; here.mkdir()
        bounded = self.session(0); self.assertEqual(bounded.wait_state(5)['error'], errno.EOVERFLOW)
        self.assertEqual(bounded.query()[2][0], errno.EAGAIN)

    def test_crashed_writer_reconciles_before_reuse(self):
        for i in range(1500):
            (self.root / f'needle-{i}').touch()
        s = self.session(); s.wait_state(); s.control(4); s.close(kill=True)
        (self.root / 'after-crash-needle').touch()
        recovered = self.session(0)
        self.assertNotEqual(recovered.initial['state'], 2)
        self.assertEqual(recovered.wait_state()['entries'], 1501)
        self.assertEqual(len(recovered.query()[0]), 1501)
        with sqlite3.connect(self.database()) as c:
            self.assertEqual(c.execute('PRAGMA integrity_check').fetchone(), ('ok',))

    def test_query_cancel_keeps_the_monitor_and_future_queries(self):
        for i in range(4000):
            (self.root / f'needle-{i:05}').touch()
        s = self.session(); s.wait_state()
        rows, status, end = s.query(cancel=True)
        self.assertLess(len(rows), 4000)
        self.assertEqual(end[0], errno.ECANCELED); self.assertEqual(status['state'], 2)
        self.assertEqual([r[0] for r in s.query(b'*needle-03000*')[0]], [b'needle-03000'])

    def test_private_cache_symlink_and_uid_guards(self):
        (self.root / 'needle').touch(); s = self.session(); s.wait_state(); s.close()
        private = self.cache / 'openpilot-search-v1'
        self.assertEqual(private.stat().st_mode & 0o777, 0o700)
        private.chmod(0o755)
        self.assertEqual(self.session(0).initial['error'], errno.EACCES); private.chmod(0o700)
        db = self.database(); original = db.with_suffix('.saved'); db.rename(original); db.symlink_to(original)
        self.assertNotEqual(self.session(0).initial['error'], 0); db.unlink(); original.rename(db)
        with sqlite3.connect(db) as c:
            c.execute('UPDATE config SET uid=?', (os.getuid() + 1,))
        self.assertNotEqual(self.session(0).initial['error'], 0)
        repaired = self.session(4); repaired.wait_state(); repaired.close()
        with sqlite3.connect(db) as c:
            c.execute('PRAGMA user_version=99')
        self.assertNotEqual(self.session(0).initial['error'], 0)

    def test_exclusive_monitor_corrupt_rebuild_and_cold_remove(self):
        (self.root / 'needle').touch(); s = self.session(); s.wait_state()
        self.assertEqual(self.session(0).initial['error'], errno.EBUSY); s.close()
        self.database().write_bytes(b'corrupt-cache')
        self.assertNotEqual(self.session(0).initial['error'], 0)
        rebuilt = self.session(4); rebuilt.wait_state()
        self.assertEqual([r[0] for r in rebuilt.query()[0]], [b'needle']); rebuilt.close()
        removed = self.session(5)
        self.assertEqual(removed.initial['state'], 0)
        self.assertFalse(self.database().exists())
        self.assertFalse(self.database().with_name(self.database().name + '.lock').exists())

    def test_own_cache_is_excluded_and_case_distinct_selections_stay_separate(self):
        (self.root / 'needle').touch()
        whole = self.session(1, self.base); first = whole.wait_state()
        self.assertGreaterEqual(first['skipped'], 1)
        time.sleep(.8)
        self.assertEqual(whole.control()['updated'], first['updated'])
        self.assertEqual([r[0] for r in whole.query()[0]], [b'files/needle'])
        whole.close()
        upper = self.base / 'Files'; upper.mkdir(); (upper / 'other-needle').touch()
        selected = self.session(1, upper); selected.wait_state()
        self.assertEqual([r[0] for r in selected.query()[0]], [b'other-needle'])
        self.assertNotEqual(self.database(upper), self.database(self.root))

    def test_malformed_open_and_symlink_ancestors_are_rejected(self):
        requests = [HEAD.pack(48, 0, 1), HEAD.pack(48, 16393, 1),
                    HEAD.pack(48, 10, 1) + struct.pack('<II', 1, 2) + b'/\0',
                    HEAD.pack(48, 11, 1) + struct.pack('<II', 1, 3) + b'../']
        for request in requests:
            p = subprocess.run([HELPER, '--index'], input=request, capture_output=True,
                               env=dict(os.environ, XDG_CACHE_HOME=str(self.cache)), timeout=5)
            self.assertNotEqual(p.returncode, 0); self.assertEqual(len(p.stdout), 28)
        link = self.base / 'link'; link.symlink_to(self.root, target_is_directory=True)
        bad = self.session(1, link)
        self.assertEqual(bad.wait_state(5)['state'], 5)
        self.assertEqual(bad.query()[0], [])


if __name__ == '__main__':
    unittest.main(verbosity=2)
