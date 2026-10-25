"""Focused Linux commit-queue ownership, durability and interruption contracts.
Only owned helper processes/temporary trees are interrupted. WSL stays running.
"""
import errno
import json
import os
import resource
from pathlib import Path
import struct
import subprocess
import tempfile
import time
import unittest
from unittest.mock import patch

import test_wsl_jobs as jobs

ROOT = Path(__file__).resolve().parents[1]
SOURCES = ['linux/fpjobs.c', 'linux/fpstage.h', 'linux/fpmetadata.h',
           'linux/fpmount.h', 'linux/fpancestry.h', 'src/wslproto.h',
           'tests/fixtures/commit_queue.c', 'tests/test_commit_queue.py', 'tests/test_wsl_jobs.py']
SELECTORS = [
    'test_bounded_width_and_durable_completion',
    'test_duplicate_names_overwrites_and_directory_boundaries',
    'test_late_source_and_destination_changes_are_refused',
    'test_failed_barriers_never_count_completed_files',
    'test_cancellation_drains_published_work_without_source_removal',
    'test_owned_process_interruption_preserves_unacknowledged_data',
    'test_failed_thread_start_and_descriptor_lifetime',
    'test_source_ack_rejection_and_control_correlation',
]
TEST_CONTRACTS = {name: {'primary': 'Linux integration',
    'owner': 'Bounded Linux copy commit queue; source ownership and durable completion',
    'sources': SOURCES} for name in SELECTORS}

class CommitQueue(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix='openpilot_commit_build_')
        cls.addClassCleanup(cls.build.cleanup)
        cls.helper = str(Path(cls.build.name)/'folderwing-wsl-commit')
        subprocess.run(['cc', '-std=c11', '-O2', '-pthread', '-I'+str(ROOT/'src'),
                        str(ROOT/'tests/fixtures/commit_queue.c'), '-o', cls.helper],
                       check=True, capture_output=True)

    def setUp(self):
        self.n = jobs.NativeJobs('runTest'); self.n.setUp(); self.addCleanup(self.n.tearDown)
        self.addCleanup(patch.stopall)
        patch.object(jobs, 'HELPER', self.helper).start()
        self.n.env['OPENPILOT_LINUX_COMMIT_WIDTH'] = '8'
        self.serial = 0

    def file(self, folder=None, name=None):
        self.serial += 1
        p = (folder or self.n.src)/(name or f'file-{self.serial}')
        p.write_bytes(bytes([self.serial % 256])*4096); p.chmod(0o640)
        return p

    def job(self, sources, **kwargs):
        return self.n.job(0, sources, self.n.dst, **kwargs)

    def test_bounded_width_and_durable_completion(self):
        sources = [self.file() for _ in range(40)]
        for width in ['1', '2', '4', '8']:
            self.n.env.update(OPENPILOT_LINUX_COMMIT_WIDTH=width, CQ_DELAY_US='30000', CQ_DEST=str(self.n.dst))
            captured = []; real = subprocess.Popen
            class Capture:
                def __init__(self, stream): self.stream = stream
                def read(self, *args):
                    data = self.stream.read(*args); captured.append(data); return data
                def close(self): self.stream.close()
            def launch(*args, **kwargs):
                p = real(*args, **kwargs); p.stderr = Capture(p.stderr); return p
            with patch.object(subprocess, 'Popen', launch):
                # NativeJobs consumes stderr on exit; record it before close.
                result = self.job(sources, policy=2)
            self.assertEqual((result['errors'], result['items'], result['done']), (0, 40, 40*4096))
            stats = json.loads(captured[0])
            self.assertEqual(stats['threads'], 0 if width == '1' else int(width))
            self.assertEqual(stats['peak_flushes'], int(width))
            self.assertEqual([stats[key] for key in ['payloads', 'parents', 'stages']], [40, 40, 40])
            for source in sources:
                target = self.n.dst/source.name
                self.assertEqual(target.read_bytes(), source.read_bytes())
                self.assertEqual(target.stat().st_mode & 0o777, 0o640)
                self.assertEqual(target.stat().st_mtime_ns, source.stat().st_mtime_ns)
            self.assertEqual(list(self.n.dst.glob('.openpilot-stage-*')), [])

    def test_duplicate_names_overwrites_and_directory_boundaries(self):
        a = self.n.src/'a'; b = self.n.src/'b'; a.mkdir(); b.mkdir()
        one = self.file(a, 'same'); two = self.file(b, 'same')
        for policy in [0, 1, 2, 3]:
            output = self.n.dst/f'p{policy}'; output.mkdir()
            (output/'same').write_bytes(b'existing')
            r = self.n.job(0, [one, two, one], output, policy=policy)
            if policy == 0:
                self.assertEqual(r['errors'], 0)
                self.assertEqual([(output/f'same ({i})').read_bytes() for i in [2, 3, 4]],
                                 [one.read_bytes(), two.read_bytes(), one.read_bytes()])
            elif policy == 1: self.assertEqual((r['errors'], r['skipped']), (0, 3))
            elif policy == 2:
                self.assertEqual(r['errors'], 0); self.assertEqual((output/'same').read_bytes(), one.read_bytes())
            else: self.assertEqual(r['errors'], 3)
        tree = self.n.src/'tree'; (tree/'nested').mkdir(parents=True)
        self.file(tree); self.file(tree/'nested'); (tree/'link').symlink_to('missing')
        os.utime(tree, ns=(1234567890123456789, 1234567890123456789))
        os.utime(tree/'nested', ns=(1234567890123456789, 1234567890123456789))
        r = self.job([tree]); self.assertEqual(r['errors'], 0)
        for suffix in ['', 'nested']:
            self.assertEqual((self.n.dst/'tree'/suffix).stat().st_mtime_ns, (tree/suffix).stat().st_mtime_ns)
        self.assertTrue((self.n.dst/'tree/nested').is_dir())
        self.assertEqual(list(self.n.dst.rglob('.openpilot-stage-*')), [])

    def test_late_source_and_destination_changes_are_refused(self):
        for subject in ['source', 'destination']:
            for mutation in ['replace', 'edit', 'metadata']:
                source = self.file(); target = self.n.dst/source.name; target.write_bytes(b'old target')
                changed = source if subject == 'source' else target
                self.n.env.update(CQ_BOUNDARY='payload', CQ_MUTATE=mutation, CQ_TARGET=str(changed))
                r = self.job([source], policy=2)
                self.assertEqual([e[1] for e in r['events']], [errno.ESTALE])
                self.assertEqual(r['done'], 0)
                if subject == 'source': self.assertEqual(target.read_bytes(), b'old target')
                else: self.assertEqual(source.read_bytes(), bytes([self.serial % 256])*4096)
                self.assertEqual(list(self.n.dst.glob('.openpilot-stage-*')), [])

    def test_failed_barriers_never_count_completed_files(self):
        for boundary in ['payload', 'parent', 'stage']:
            source = self.file(); target = self.n.dst/source.name; target.write_bytes(b'old target')
            self.n.env.update(CQ_BOUNDARY=boundary, CQ_DEST=str(self.n.dst), CQ_FAIL='1')
            r = self.job([source], policy=2)
            self.assertEqual([e[1] for e in r['events']], [errno.EIO])
            self.assertEqual(r['done'], 0)
            self.assertEqual(target.read_bytes(), b'old target' if boundary == 'payload' else source.read_bytes())
            self.assertTrue(source.exists())

    def interrupted(self, boundary, kill):
        source = self.file(); target = self.n.dst/source.name
        marker = self.n.root/f'marker-{self.serial}'; release = self.n.root/f'release-{self.serial}'
        env = dict(self.n.env, CQ_BOUNDARY=boundary, CQ_DEST=str(self.n.dst), CQ_MARK=str(marker), CQ_RELEASE=str(release))
        p = subprocess.Popen([self.helper, '--jobs'], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)
        self.addCleanup(lambda: p.poll() is None and (p.kill(), p.wait()))
        def message():
            h = p.stdout.read(12); self.assertEqual(len(h), 12)
            typ, size, ident = jobs.HEAD.unpack(h)
            return typ, p.stdout.read(size)
        try:
            self.assertEqual(message()[0], 1)
            body = struct.pack('<IIII', 0, 3, 1, 0)
            for path in [self.n.dst, source]:
                raw = os.fsencode(path); body += struct.pack('<II', len(raw), 0)+raw
            p.stdin.write(jobs.HEAD.pack(10, len(body), 1)+body); p.stdin.flush()
            while True:
                typ, data = message()
                if typ == 11 and jobs.PROGRESS.unpack(data[:48])[2] == 2: break
            p.stdin.write(jobs.HEAD.pack(14, 0, 1)); p.stdin.flush()
            deadline = time.monotonic()+5
            while not marker.exists():
                self.assertIsNone(p.poll()); self.assertLess(time.monotonic(), deadline); time.sleep(.005)
            if kill:
                p.kill(); p.wait(timeout=5)
                # No final completion message can have preceded this barrier.
                extra = p.stdout.read()
                while extra:
                    typ, size, ident = jobs.HEAD.unpack(extra[:12]); body = extra[12:12+size]; extra = extra[12+size:]
                    if typ == 11: self.assertLess(jobs.PROGRESS.unpack(body[:48])[2], 3)
            else:
                p.stdin.write(jobs.HEAD.pack(13, 0, 1)); p.stdin.flush(); release.touch()
                while True:
                    typ, data = message()
                    if typ == 11:
                        values = jobs.PROGRESS.unpack(data[:48])
                        if values[2] >= 3:
                            self.assertEqual((values[2], values[0]), (4, 0)); break
                p.wait(timeout=5); self.assertEqual(p.returncode, 0)
            self.assertTrue(source.exists())
            if boundary == 'payload':
                self.assertFalse(target.exists())
                if kill: self.assertTrue(list(self.n.dst.glob('.openpilot-stage-*/payload')))
            else: self.assertEqual(target.read_bytes(), source.read_bytes())
        finally:
            if p.poll() is None: p.kill(); p.wait()
            p.stdin.close(); p.stdout.close(); p.stderr.close()

    def test_cancellation_drains_published_work_without_source_removal(self):
        for boundary in ['payload', 'parent']: self.interrupted(boundary, False)

    def test_owned_process_interruption_preserves_unacknowledged_data(self):
        # Retain killed-process artifacts for review, outside auto-cleanup.
        held = tempfile.mkdtemp(prefix='openpilot_commit_interrupted_')
        self.n.dst = Path(held)
        (self.n.dst/'.openpilot-owner').write_text('Owned commit queue interruption fixture; preserve for review.\n')
        for boundary in ['payload', 'parent']: self.interrupted(boundary, True)

    def test_failed_thread_start_and_descriptor_lifetime(self):
        source = self.file()
        for fail in ['1', '3']:
            self.n.env['CQ_THREAD_FAIL'] = fail
            r = self.job([source]); self.assertEqual([e[1] for e in r['events']], [errno.EAGAIN])
            self.assertFalse((self.n.dst/source.name).exists())
        self.n.env.pop('CQ_THREAD_FAIL')
        sources = [source]+[self.file() for _ in range(200)]
        real = subprocess.Popen
        def limited(*args, **kwargs):
            kwargs['preexec_fn'] = lambda: resource.setrlimit(resource.RLIMIT_NOFILE, (96, 96))
            return real(*args, **kwargs)
        for _ in range(3):
            with patch.object(subprocess, 'Popen', limited):
                r = self.job(sources, policy=2)
            self.assertEqual((r['errors'], r['items']), (0, len(sources)))
            self.assertEqual(list(self.n.dst.glob('.openpilot-stage-*')), [])

    def test_source_ack_rejection_and_control_correlation(self):
        for reply in [errno.ENOTSUP, (60, b'bad'), (60, struct.pack('<i', 0)), (60, struct.pack('<i', errno.ECANCELED))]:
            source = self.file()
            windows = subprocess.check_output(['wslpath', '-w', str(source)]).decode().strip()
            r = self.job([windows], windows=(1,), source_reply=lambda n, root, suffix: 0 if n == 1 else reply)
            if reply == (60, struct.pack('<i', 0)):
                self.assertEqual(r['errors'], 0); self.assertEqual((self.n.dst/source.name).read_bytes(), source.read_bytes())
            else:
                self.assertFalse((self.n.dst/source.name).exists()); self.assertEqual(r['done'], 0)
            self.assertTrue(source.exists())

if __name__ == '__main__': unittest.main(verbosity=2)
