"""Uncatchable process exits, actual Linux writers; not VM/power-loss tests."""
import importlib.util
import os
from pathlib import Path
import select
import shlex
import signal
import struct
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import test_cross_distro as cross
import test_wsl_jobs as native

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('staging_review', ROOT/'tools/staging-review.py')
review = importlib.util.module_from_spec(spec); spec.loader.exec_module(review)
PAYLOAD = b'complete selected bytes'*100


class TransferCrash(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix='openpilot_crash_build_')
        cls.addClassCleanup(cls.build.cleanup)
        cls.helper = str(Path(cls.build.name)/'folderwing-wsl-crash')
        subprocess.run([*shlex.split(os.environ.get('CC', 'cc')), '-std=c11', '-O2', '-pthread',
                        '-I'+str(ROOT/'src'), str(ROOT/'linux/fpjobs.c'),
                        str(ROOT/'linux/fptransfer.c'), str(ROOT/'tests/fixtures/transfer_crash.c'),
                        '-Wl,--wrap=fsync', '-Wl,--wrap=write', '-Wl,--wrap=unlinkat', '-o', cls.helper],
                       check=True, capture_output=True)

    def setUp(self):
        self.c = cross.CrossDistro('runTest'); self.c.setUp(); self.addCleanup(self.c.tearDown)
        self.other = tempfile.TemporaryDirectory(prefix='openpilot_crash_', dir='/dev/shm')
        self.addCleanup(self.other.cleanup)
        self.dest = Path(self.other.name)
        self.assertNotEqual(self.c.src.stat().st_dev, self.dest.stat().st_dev)
        p = patch.object(cross, 'HELPER', self.helper); p.start(); self.addCleanup(p.stop)

    def spawn(self, mode, env):
        p = subprocess.Popen([self.helper, mode], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE, env=dict(os.environ, **env))
        self.c.children.append(p)
        typ, _, _ = self.c.receive(p); self.assertEqual(typ, 1)
        return p

    def killed(self, p):
        p.wait(timeout=10); self.assertEqual(p.returncode, -signal.SIGKILL, p.stderr.read())
        self.assertEqual(p.stdout.read(), b'', 'no successful ACK/progress after crash boundary')

    def native_crash(self, source, target, env, move):
        p = self.spawn('--jobs', env)
        body = struct.pack('<IIII', int(move), 2, 1, 0)
        for path in [target.parent, source]:
            raw = os.fsencode(path); body += struct.pack('<II', len(raw), 0)+raw
        self.c.send(p, 10, body)
        continued = False
        while True:
            # Progress before the boundary is legal; EOF must be SIGKILL, never completion.
            if not select.select([p.stdout], [], [], 10)[0]: self.fail('crash boundary not reached')
            raw = os.read(p.stdout.fileno(), 12)
            if not raw: break
            self.assertEqual(len(raw), 12)
            typ, size, _ = cross.HEAD.unpack(raw)
            body = b''
            while len(body) < size:
                part = os.read(p.stdout.fileno(), size-len(body)); self.assertTrue(part); body += part
            self.assertEqual(typ, 11, (typ, body))
            state = native.PROGRESS.unpack(body[:48])[2]
            self.assertLess(state, 3)
            if state == 2 and not continued:
                self.c.send(p, 14); continued = True
        self.killed(p)

    def relay(self, source, dest, receiver_env=None, sender_env=None, lose_ack=False):
        receiver = self.spawn('--import', receiver_env or {})
        raw = os.fsencode(dest)
        self.c.send(receiver, cross.TRANSFER, struct.pack('<IIII', 1, 2, len(raw), 0)+raw)
        self.assertEqual(cross.ACK.unpack(self.c.receive(receiver)[2])[0], 0)
        with patch.dict(os.environ, sender_env or {}): sender = self.c.start(False, source, 1, 2)
        while True:
            typ, _, body = self.c.receive(sender)
            if typ == 11:
                state = cross.PROGRESS.unpack(body[:48])[2]; self.assertLess(state, 3)
                if state == 2: self.c.send(sender, 14)
                continue
            self.assertIn(typ, [cross.NODE_TYPE, cross.DATA, cross.FILE_END])
            self.c.send(receiver, typ, body)
            if typ == cross.DATA and not (receiver_env or {}).get('FP_CRASH_WRITE'): continue
            if receiver_env and (typ != cross.NODE_TYPE or source.is_symlink()):
                self.killed(receiver)
                sender.kill(); sender.wait(timeout=5)
                return
            if typ == cross.DATA: continue
            ack_type, _, ack = self.c.receive(receiver); self.assertEqual(ack_type, cross.ACK_TYPE)
            error, skipped, committed, _ = cross.ACK.unpack(ack)
            self.assertEqual((error, skipped), (0, 0))
            final = typ == cross.FILE_END or source.is_symlink()
            if final:
                self.assertEqual(committed, 1)
                if lose_ack:
                    receiver.kill(); receiver.wait(timeout=5)
                    sender.kill(); sender.wait(timeout=5)
                    return
            self.c.send(sender, cross.ACK_TYPE, ack)
            if final:
                self.killed(sender)
                receiver.kill(); receiver.wait(timeout=5)
                return

    def observed(self, target, expected, staged):
        self.assertEqual(target.read_bytes(), expected)
        result = review.review(target.parent)
        self.assertEqual(len(result['candidates']), staged)
        self.assertFalse(result['errors'])
        return result

    def test_native_partial_prepublication_and_postpublication_crashes(self):
        for move in [False, True]:
            for boundary in ['partial', 'prepublish', 'published']:
                with self.subTest(move=move, boundary=boundary):
                    source = self.c.src/(str(move)+boundary); source.write_bytes(PAYLOAD)
                    target = self.dest/source.name; target.write_bytes(b'previous target')
                    stage = str(self.dest/'.openpilot-stage-*/payload')
                    env = {'FP_CRASH_WRITE': stage} if boundary == 'partial' else {
                        'FP_CRASH_SYNC': str(self.dest) if boundary == 'published' else stage}
                    self.native_crash(source, target, env, move)
                    self.assertEqual(source.read_bytes(), PAYLOAD)
                    self.observed(target, PAYLOAD if boundary == 'published' else b'previous target', 1)
                    workspace = next(self.dest.glob('.openpilot-stage-*'))
                    if boundary == 'published': self.assertFalse((workspace/'payload').exists())
                    else: self.assertEqual((workspace/'payload').read_bytes(), PAYLOAD[:len(PAYLOAD)//2] if boundary == 'partial' else PAYLOAD)
                    # Remove only our exact known fixture after its assertions.
                    if (workspace/'payload').exists(): (workspace/'payload').unlink()
                    workspace.rmdir()

    def test_import_partial_prepublication_and_postpublication_crashes(self):
        for boundary in ['partial', 'prepublish', 'published']:
            with self.subTest(boundary=boundary):
                source = self.c.src/boundary; source.write_bytes(PAYLOAD)
                dest = self.dest/boundary; dest.mkdir(); target = dest/source.name; target.write_bytes(b'previous target')
                stage = str(dest/'.openpilot-stage-*/payload')
                env = {'FP_CRASH_WRITE': stage} if boundary == 'partial' else {
                    'FP_CRASH_SYNC': str(dest) if boundary == 'published' else stage,
                    'FP_CRASH_NTH': '2' if boundary == 'prepublish' else '1'}
                self.relay(source, dest, receiver_env=env)
                self.assertEqual(source.read_bytes(), PAYLOAD)
                self.observed(target, PAYLOAD if boundary == 'published' else b'previous target', 1)

    def test_symlink_crash_and_lost_ack_preserve_source(self):
        for boundary in ['prepublish', 'published', 'lost-ack']:
            with self.subTest(boundary=boundary):
                source = self.c.src/boundary; source.symlink_to('literal-missing-target')
                dest = self.dest/boundary; dest.mkdir(); target = dest/source.name
                env = None if boundary == 'lost-ack' else {'FP_CRASH_SYNC':
                    str(dest) if boundary == 'published' else str(dest/'.openpilot-stage-*')}
                self.relay(source, dest, receiver_env=env, lose_ack=boundary == 'lost-ack')
                self.assertEqual(os.readlink(source), 'literal-missing-target')
                if boundary == 'prepublish': self.assertFalse(target.is_symlink())
                else: self.assertEqual(os.readlink(target), 'literal-missing-target')

    def test_lost_file_ack_and_restarted_importer_leave_completed_data(self):
        source = self.c.src/'selected'; source.write_bytes(PAYLOAD)
        target = self.dest/source.name
        self.relay(source, self.dest, lose_ack=True)
        before = self.observed(target, PAYLOAD, 1)
        receiver = self.c.start(True, self.dest, 1, 2)
        self.assertEqual(cross.ACK.unpack(self.c.receive(receiver)[2])[0], 0)
        self.c.send(receiver, cross.QUIT); receiver.wait(timeout=5); self.assertEqual(receiver.returncode, 0)
        self.assertEqual(self.observed(target, PAYLOAD, 1)['candidates'], before['candidates'])
        self.assertEqual(source.read_bytes(), PAYLOAD)

    def test_source_unlink_crashes_keep_published_destination(self):
        for engine in ['native', 'cross']:
            for before in [True, False]:
                with self.subTest(engine=engine, before=before):
                    source = self.c.src/(engine+str(before)); source.write_bytes(PAYLOAD)
                    target = self.dest/source.name
                    env = {'FP_CRASH_UNLINK' if before else 'FP_CRASH_SYNC': str(source.parent)}
                    if engine == 'native': self.native_crash(source, target, env, True)
                    else: self.relay(source, self.dest, sender_env=env)
                    self.assertEqual(source.exists(), before); self.assertEqual(target.read_bytes(), PAYLOAD)

    def test_native_symlink_crashes_keep_literal_sources(self):
        for boundary in ['prepublish', 'published']:
            with self.subTest(boundary=boundary):
                source = self.c.src/boundary; source.symlink_to('literal-missing-target')
                target = self.dest/source.name
                self.native_crash(source, target, {'FP_CRASH_SYNC': str(self.dest) if boundary == 'published'
                    else str(self.dest/'.openpilot-stage-*')}, True)
                self.assertEqual(os.readlink(source), 'literal-missing-target')
                if boundary == 'published': self.assertEqual(os.readlink(target), 'literal-missing-target')
                else: self.assertFalse(target.is_symlink())

    def test_incremental_move_crash_keeps_completed_child_and_remaining_source(self):
        source = self.c.src/'tree'; source.mkdir()
        for name in ['one', 'two']: (source/name).write_bytes(PAYLOAD)
        target = self.dest/source.name
        self.native_crash(source, target, {'FP_CRASH_SYNC': str(target/'.openpilot-stage-*/payload'), 'FP_CRASH_NTH': '2'}, True)
        remaining = list(source.iterdir()); self.assertEqual(len(remaining), 1)
        completed = [p for p in target.iterdir() if not p.name.startswith('.openpilot-stage-')]
        self.assertEqual(len(completed), 1); self.assertNotEqual(remaining[0].name, completed[0].name)
        self.assertEqual(remaining[0].read_bytes(), PAYLOAD); self.assertEqual(completed[0].read_bytes(), PAYLOAD)
        self.assertEqual(len(review.review(target)['candidates']), 1)


if __name__ == '__main__':
    unittest.main()
