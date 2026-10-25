"""Linux persistence contracts using production transfer code and link-time faults.

Run: python3 tests/test_transfer_durability.py
Requires a native C compiler and distinct /tmp and /dev/shm filesystems. tmpfs
exercises EXDEV paths only; these tests do not simulate power loss. No product
environment switches or injected failures are compiled into the shipped helper.
"""
import errno
import json
import os
import signal
import struct
import time
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import test_cross_distro as cross
import test_wsl_jobs as native

ROOT = Path(__file__).resolve().parents[1]

# Exact new-selector ownership and additive source-impact contracts.
TEST_CONTRACTS = {'test_existing_nested_merge_cancellation_before_descent': {'owner': 'Linux native and streamed '
                                                                     'nested-merge prefix '
                                                                     'durability',
                                                            'primary': 'Linux integration',
                                                            'sources': ['linux/fpjobs.c',
                                                                        'linux/fptransfer.c',
                                                                        'linux/fpstage.h',
                                                                        'linux/fpmount.h',
                                                                        'linux/fpmetadata.h',
                                                                        'linux/fpancestry.h',
                                                                        'linux/TRANSFER_DURABILITY.md',
                                                                        'src/wslproto.h',
                                                                        'tests/fixtures/transfer_durability.c',
                                                                        'tests/test_transfer_durability.py',
                                                                        'tests/test_wsl_jobs.py',
                                                                        'tests/test_cross_distro.py']},
 'test_existing_nested_merge_copy_control_keeps_sources': {'owner': 'Linux native and streamed '
                                                                    'nested-merge prefix '
                                                                    'durability',
                                                           'primary': 'Linux integration',
                                                           'sources': ['linux/fpjobs.c',
                                                                       'linux/fptransfer.c',
                                                                       'linux/fpstage.h',
                                                                       'linux/fpmount.h',
                                                                       'linux/fpmetadata.h',
                                                                       'linux/fpancestry.h',
                                                                       'linux/TRANSFER_DURABILITY.md',
                                                                       'src/wslproto.h',
                                                                       'tests/fixtures/transfer_durability.c',
                                                                       'tests/test_transfer_durability.py',
                                                                       'tests/test_wsl_jobs.py',
                                                                       'tests/test_cross_distro.py']},
 'test_existing_nested_merge_fences_precede_child_source_removal': {'owner': 'Linux native and '
                                                                             'streamed '
                                                                             'nested-merge prefix '
                                                                             'durability',
                                                                    'primary': 'Linux integration',
                                                                    'sources': ['linux/fpjobs.c',
                                                                                'linux/fptransfer.c',
                                                                                'linux/fpstage.h',
                                                                                'linux/fpmount.h',
                                                                                'linux/fpmetadata.h',
                                                                                'linux/fpancestry.h',
                                                                                'linux/TRANSFER_DURABILITY.md',
                                                                                'src/wslproto.h',
                                                                                'tests/fixtures/transfer_durability.c',
                                                                                'tests/test_transfer_durability.py',
                                                                                'tests/test_wsl_jobs.py',
                                                                                'tests/test_cross_distro.py']},
 'test_existing_nested_merge_initial_fence_errors_preserve_children': {'owner': 'Linux native and '
                                                                                'streamed '
                                                                                'nested-merge '
                                                                                'prefix durability',
                                                                       'primary': 'Linux '
                                                                                  'integration',
                                                                       'sources': ['linux/fpjobs.c',
                                                                                   'linux/fptransfer.c',
                                                                                   'linux/fpstage.h',
                                                                                   'linux/fpmount.h',
                                                                                   'linux/fpmetadata.h',
                                                                                   'linux/fpancestry.h',
                                                                                   'linux/TRANSFER_DURABILITY.md',
                                                                                   'src/wslproto.h',
                                                                                   'tests/fixtures/transfer_durability.c',
                                                                                   'tests/test_transfer_durability.py',
                                                                                   'tests/test_wsl_jobs.py',
                                                                                   'tests/test_cross_distro.py']},
 'test_existing_nested_merge_interrupted_fence_retries': {'owner': 'Linux native and streamed '
                                                                   'nested-merge prefix durability',
                                                          'primary': 'Linux integration',
                                                          'sources': ['linux/fpjobs.c',
                                                                      'linux/fptransfer.c',
                                                                      'linux/fpstage.h',
                                                                      'linux/fpmount.h',
                                                                      'linux/fpmetadata.h',
                                                                      'linux/fpancestry.h',
                                                                      'linux/TRANSFER_DURABILITY.md',
                                                                      'src/wslproto.h',
                                                                      'tests/fixtures/transfer_durability.c',
                                                                      'tests/test_transfer_durability.py',
                                                                      'tests/test_wsl_jobs.py',
                                                                      'tests/test_cross_distro.py']}}


class TransferDurability(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix='openpilot_durability_build_')
        cls.addClassCleanup(cls.build.cleanup)
        cls.helper = str(Path(cls.build.name)/'folderwing-wsl-faults')
        source = Path(os.environ.get('FP_DURABILITY_CODE_ROOT', ROOT))
        subprocess.run([*shlex.split(os.environ.get('CC', 'cc')), '-std=c11', '-O2', '-pthread',
                        '-I'+str(ROOT/'src'), '-I'+str(ROOT/'linux'),
                        str(source/'linux/fpjobs.c'), str(source/'linux/fptransfer.c'),
                        str(ROOT/'tests/fixtures/transfer_durability.c'),
                        '-Wl,--wrap=fsync', '-o', cls.helper], check=True, capture_output=True)

    def setUp(self):
        self.n = native.NativeJobs('runTest'); self.n.setUp(); self.addCleanup(self.n.tearDown)
        self.c = cross.CrossDistro('runTest'); self.c.setUp(); self.addCleanup(self.c.tearDown)
        self.volume = tempfile.TemporaryDirectory(prefix='openpilot_durability_', dir='/dev/shm')
        self.addCleanup(self.volume.cleanup)
        self.other = Path(self.volume.name)
        self.assertNotEqual(self.n.src.stat().st_dev, self.other.stat().st_dev,
                            'EXDEV contract requires two filesystems; do not silently skip it')
        for module in [native, cross]:
            helper_patch = patch.object(module, 'HELPER', self.helper)
            helper_patch.start(); self.addCleanup(helper_patch.stop)
        self.serial = 0

    def case(self, engine, kind='file', replace=False, merge=False):
        self.serial += 1
        h = self.c if engine == 'cross' else self.n
        name = 'node-'+str(self.serial)
        source = h.src/name
        if kind == 'file': source.write_bytes(b'complete transferred bytes')
        elif kind == 'link': source.symlink_to('literal-missing-target')
        else: source.mkdir(mode=0o750)
        dest = self.c.dst if engine == 'cross' else self.other
        target = dest/name
        if replace:
            if kind == 'file': target.write_bytes(b'old target')
            else: target.symlink_to('old-target')
        if merge: target.mkdir()
        return source, target

    def run_transfer(self, engine, source, target, pattern='', nth=1, error=errno.EIO, op=1, source_state=None, target_state=None):
        log = self.n.root/('sync-'+str(self.serial)+'.log')
        if log.exists(): log.unlink()
        env = dict(FP_DURABILITY_MATCH=str(pattern), FP_DURABILITY_NTH=str(nth),
                   FP_DURABILITY_ERRNO=str(error), FP_DURABILITY_LOG=str(log),
                   FP_DURABILITY_SOURCE_STATE=str(source_state or source), FP_DURABILITY_TARGET_STATE=str(target_state or target))
        with patch.dict(os.environ, env), patch.dict(self.n.env, env):
            if engine == 'cross': result = self.c.transfer(source, op=op, policy=2, destination=target.parent)
            else: result = self.n.job(op, [source], target.parent, policy=2)
        records = []
        if log.exists():
            for line in log.read_text().splitlines():
                err, src, dst, path = line.split('\t', 3)
                records.append((int(err), int(src), int(dst), path))
        evidence = os.environ.get('OPENPILOT_N04_EVIDENCE')
        if evidence and source_state is not None:
            receipt = dict(engine=engine, op=op, pattern=str(pattern), nth=nth, injected_errno=error,
                           source=str(source_state), target=str(target_state), result=result,
                           fsync_records=records)
            receipt['wire_bytes_encoding'] = 'hex'
            Path(evidence, 'merge-'+self._testMethodName+'-'+str(os.getpid())+'-'+str(self.serial)+'.json').write_text(json.dumps(receipt,sort_keys=True,default=lambda value: value.hex())+'\n')
        return result, records

    def failed(self, result, records, error=errno.EIO):
        self.assertEqual((result['state'], result['errors'], result['skipped']), (3, 1, 0))
        self.assertEqual([e[1] for e in result['events']], [error])
        self.assertEqual(sum(r[0] == error for r in records), 1, records)

    def assert_payload(self, target, kind):
        if kind == 'file': self.assertEqual(target.read_bytes(), b'complete transferred bytes')
        elif kind == 'link': self.assertEqual(os.readlink(target), 'literal-missing-target')
        else: self.assertTrue(target.is_dir())

    def test_regular_file_flush_failure_retains_source_and_old_target(self):
        for engine in ['native', 'cross']:
            for op in [0, 1]:
                for error in [errno.EIO, errno.EINVAL, errno.ENOTSUP]:
                    for nth in ([1, 2] if engine == 'cross' else [1]):
                        with self.subTest(engine=engine, op=op, error=error, nth=nth):
                            source, target = self.case(engine, replace=True)
                            r, log = self.run_transfer(engine, source, target,
                                target.parent/'.openpilot-stage-*/payload', nth, error, op)
                            self.failed(r, log, error)
                            self.assert_payload(source, 'file'); self.assertEqual(target.read_bytes(), b'old target')
                            self.assertFalse(list(target.parent.glob('.openpilot-stage-*')))

    def test_published_destination_flush_failure_keeps_both_copies(self):
        for engine in ['native', 'cross']:
            for kind in ['file', 'link']:
                for boundary in ['destination', 'workspace']:
                    with self.subTest(engine=engine, kind=kind, boundary=boundary):
                        source, target = self.case(engine, kind, replace=True)
                        pattern = target.parent if boundary == 'destination' else target.parent/'.openpilot-stage-*'
                        nth = 2 if boundary == 'workspace' and kind == 'link' else 1
                        r, log = self.run_transfer(engine, source, target, pattern, nth)
                        self.failed(r, log); self.assert_payload(source, kind); self.assert_payload(target, kind)
                        self.assertFalse(list(target.parent.glob('.openpilot-stage-*')))

    def test_symlink_prepublication_flush_failure_preserves_old_target(self):
        for engine in ['native', 'cross']:
            with self.subTest(engine=engine):
                source, target = self.case(engine, 'link', replace=True)
                r, log = self.run_transfer(engine, source, target, target.parent/'.openpilot-stage-*')
                self.failed(r, log); self.assert_payload(source, 'link')
                self.assertEqual(os.readlink(target), 'old-target')

    def test_source_parent_flush_failure_reports_already_removed_source(self):
        for engine in ['native', 'cross']:
            for kind in ['file', 'link', 'directory']:
                with self.subTest(engine=engine, kind=kind):
                    source, target = self.case(engine, kind)
                    r, log = self.run_transfer(engine, source, target, source.parent)
                    self.failed(r, log); self.assertFalse(os.path.lexists(source)); self.assert_payload(target, kind)
                    self.assertEqual([x[1:3] for x in log if x[0]], [(0, 1)])

    def test_new_directory_fences_precede_child_source_removal(self):
        for engine in ['native', 'cross']:
            for boundary in ['directory', 'parent']:
                for error in [errno.EIO, errno.EINVAL, errno.ENOTSUP]:
                    with self.subTest(engine=engine, boundary=boundary, error=error):
                        source, target = self.case(engine, 'directory')
                        (source/'child').write_bytes(b'not moved yet')
                        r, log = self.run_transfer(engine, source, target,
                            target if boundary == 'directory' else target.parent, error=error)
                        self.failed(r, log, error)
                        self.assertEqual((source/'child').read_bytes(), b'not moved yet')
                        self.assertEqual(list(target.iterdir()), [])

    def test_final_directory_metadata_fence_failure_keeps_empty_source(self):
        for engine in ['native', 'cross']:
            for boundary in ['directory', 'parent']:
                with self.subTest(engine=engine, boundary=boundary):
                    source, target = self.case(engine, 'directory')
                    r, log = self.run_transfer(engine, source, target,
                        target if boundary == 'directory' else target.parent, nth=2)
                    self.failed(r, log); self.assertTrue(source.is_dir()); self.assertTrue(target.is_dir())
                    self.assertEqual(target.stat().st_mode & 0o777, 0o750)

    def test_final_directory_failure_reports_partial_move_truthfully(self):
        for engine in ['native', 'cross']:
            with self.subTest(engine=engine):
                source, target = self.case(engine, 'directory'); (source/'child').write_bytes(b'committed child')
                # Initial directory fence, child publication, final metadata fence.
                r, log = self.run_transfer(engine, source, target, target, nth=3)
                self.failed(r, log); self.assertTrue(source.is_dir()); self.assertEqual(list(source.iterdir()), [])
                self.assertEqual((target/'child').read_bytes(), b'committed child')

    def test_merged_directory_final_fence_is_required(self):
        for engine in ['native', 'cross']:
            with self.subTest(engine=engine):
                source, target = self.case(engine, 'directory', merge=True)
                # Existing merges have both an initial fence and this final fence.
                r, log = self.run_transfer(engine, source, target, target, nth=2)
                self.failed(r, log); self.assertTrue(source.is_dir()); self.assertTrue(target.is_dir())

    def nested_merge(self, route, kind='file'):
        engine = 'cross' if route == 'streamed' else 'native'
        source, target = self.case(engine, 'directory', merge=True)
        if route == 'rename':
            target.rmdir(); target = self.n.dst/source.name; target.mkdir()
        source_leaf = source/'M'/'N'/'payload'; source_leaf.parent.mkdir(parents=True)
        target_leaf = target/'M'/'N'/'payload'; target_leaf.parent.mkdir(parents=True)
        if kind == 'link': source_leaf.symlink_to('literal-unresolved-target')
        else: source_leaf.write_bytes(b'not moved yet')
        sentinel = target_leaf.with_name('unselected'); sentinel.write_bytes(b'keep destination sibling')
        return engine, source, target, source_leaf, target_leaf, sentinel

    def assert_merge_leaf(self, path, kind):
        self.assertTrue(os.path.lexists(path), str(path))
        if kind == 'link': self.assertEqual(os.readlink(path), 'literal-unresolved-target')
        else: self.assertEqual(path.read_bytes(), b'not moved yet')

    def test_existing_nested_merge_fences_precede_child_source_removal(self):
        for route in ['rename', 'copy-move', 'streamed']:
            for kind in ['file', 'link']:
                for replace in [False, True]:
                    with self.subTest(route=route, kind=kind, replace=replace):
                        engine,source,target,leaf,out,sentinel = self.nested_merge(route,kind)
                        if replace:
                            if kind == 'link': out.symlink_to('previous-unresolved-target')
                            else: out.write_bytes(b'previous destination')
                        result,log = self.run_transfer(engine,source,target,source_state=leaf,target_state=out)
                        self.assertEqual((result['state'],result['errors']),(3,0))
                        self.assertFalse(source.exists()); self.assert_merge_leaf(out,kind)
                        self.assertEqual(sentinel.read_bytes(),b'keep destination sibling')
                        initial = [target,target.parent,target/'M',target,target/'M'/'N',target/'M']
                        positions=[]
                        for path in initial:
                            start=positions[-1]+1 if positions else 0
                            position=next((i for i in range(start,len(log)) if log[i][0]==0 and log[i][3]==str(path)),None)
                            self.assertIsNotNone(position, (path,log)); positions.append(position)
                        self.assertTrue(all(row[1] for row in log[:positions[-1]+1]),log)
                        removal=next(i for i,row in enumerate(log) if not row[1])
                        self.assertLess(positions[-1],removal,log)
                        self.assertFalse(list(target.rglob('.openpilot-stage-*')))

    def test_existing_nested_merge_initial_fence_errors_preserve_children(self):
        for route in ['rename', 'copy-move', 'streamed']:
            for boundary in range(6):
                for error in [errno.EIO,errno.EACCES,errno.ENOTSUP]:
                    with self.subTest(route=route,boundary=boundary,error=error):
                        engine,source,target,leaf,out,sentinel = self.nested_merge(route)
                        out.write_bytes(b'previous destination')
                        fences=[(target,1),(target.parent,1),(target/'M',1),(target,2),(target/'M'/'N',1),(target/'M',2)]
                        match,nth=fences[boundary]
                        result,log=self.run_transfer(engine,source,target,match,nth,error,source_state=leaf,target_state=out)
                        self.failed(result,log,error); self.assert_merge_leaf(leaf,'file')
                        self.assertEqual(out.read_bytes(),b'previous destination')
                        self.assertEqual(sentinel.read_bytes(),b'keep destination sibling')
                        self.assertTrue(all(row[1] for row in log),log)
                        self.assertFalse(list(target.rglob('.openpilot-stage-*')))

    def test_existing_nested_merge_copy_control_keeps_sources(self):
        for route in ['rename', 'copy-move', 'streamed']:
            with self.subTest(route=route):
                engine,source,target,leaf,out,sentinel=self.nested_merge(route)
                result,log=self.run_transfer(engine,source,target,op=0,source_state=leaf,target_state=out)
                self.assertEqual(result['errors'],0); self.assert_merge_leaf(leaf,'file'); self.assert_merge_leaf(out,'file')
                self.assertTrue(all(row[1] for row in log)); self.assertEqual(sentinel.read_bytes(),b'keep destination sibling')

    def test_existing_nested_merge_interrupted_fence_retries(self):
        for route in ['rename', 'copy-move', 'streamed']:
            with self.subTest(route=route):
                engine,source,target,leaf,out,sentinel=self.nested_merge(route); match=out.parent
                result,log=self.run_transfer(engine,source,target,match,error=errno.EINTR,source_state=leaf,target_state=out)
                self.assertEqual(result['errors'],0); self.assertFalse(source.exists()); self.assert_merge_leaf(out,'file')
                interrupted=next(i for i,row in enumerate(log) if row[0]==errno.EINTR)
                self.assertEqual(log[interrupted],(errno.EINTR,1,0,str(match)))
                self.assertEqual(log[interrupted+1],(0,1,0,str(match)))
                self.assertLess(interrupted+1,next(i for i,row in enumerate(log) if not row[1]))

    def test_existing_nested_merge_cancellation_before_descent(self):
        for depth in range(3):
            with self.subTest(depth=depth):
                engine,source,target,leaf,out,sentinel=self.nested_merge('rename'); out.write_bytes(b'previous destination')
                log=self.n.root/('cancel-'+str(self.serial)+'.log')
                match=[target,target/'M',target/'M'/'N'][depth]
                env=dict(self.n.env,FP_DURABILITY_LOG=str(log),FP_DURABILITY_MATCH=str(match),FP_DURABILITY_NTH='1',FP_DURABILITY_ERRNO='0',FP_DURABILITY_STOP='1',FP_DURABILITY_SOURCE_STATE=str(leaf),FP_DURABILITY_TARGET_STATE=str(out))
                p=subprocess.Popen([self.helper,'--jobs'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,env=env); self.c.children.append(p)
                try:
                    self.assertEqual(self.c.receive(p)[0],1)
                    body=struct.pack('<IIII',1,2,1,0)
                    for path in [target.parent,source]:
                        raw=os.fsencode(path);body+=struct.pack('<II',len(raw),0)+raw
                    self.c.send(p,10,body)
                    while True:
                        typ,_,data=self.c.receive(p);self.assertEqual(typ,11)
                        if cross.PROGRESS.unpack(data[:48])[2]==2:break
                    self.c.send(p,14);deadline=time.monotonic()+5
                    while time.monotonic()<deadline:
                        pid,status=os.waitpid(p.pid,os.WNOHANG|os.WUNTRACED)
                        if pid:self.assertTrue(os.WIFSTOPPED(status));break
                        time.sleep(.01)
                    else:self.fail('owned helper did not reach its initial merge fence')
                    self.assert_merge_leaf(leaf,'file');self.assertEqual(out.read_bytes(),b'previous destination')
                    self.c.send(p,13);os.kill(p.pid,signal.SIGCONT)
                    while True:
                        typ,_,data=self.c.receive(p);self.assertEqual(typ,11)
                        state=cross.PROGRESS.unpack(data[:48])[2]
                        if state>=3:break
                    self.assertEqual(state,4);self.assertEqual(cross.PROGRESS.unpack(data[:48])[5],0);p.wait(timeout=5);self.assertEqual(p.returncode,0)
                    self.assert_merge_leaf(leaf,'file');self.assertEqual(out.read_bytes(),b'previous destination')
                    self.assertEqual(sentinel.read_bytes(),b'keep destination sibling')
                    evidence=os.environ.get('OPENPILOT_N04_EVIDENCE')
                    if evidence:Path(evidence,'cancel-'+str(os.getpid())+'-'+str(self.serial)+'.json').write_text(json.dumps(dict(state=state,errors=0,depth=depth,source=str(leaf),target=str(out),fsync_log=log.read_text()),sort_keys=True)+'\n')
                finally:
                    if p.poll() is None:p.kill();p.wait(timeout=5)

    def test_successful_file_move_orders_fences_before_source_removal(self):
        for engine in ['native', 'cross']:
            with self.subTest(engine=engine):
                source, target = self.case(engine)
                r, log = self.run_transfer(engine, source, target)
                self.assertEqual(r['errors'], 0); self.assertFalse(source.exists()); self.assert_payload(target, 'file')
                self.assertEqual([x[0] for x in log], [0]*len(log))
                paths = [x[3] for x in log]
                payload = [i for i, p in enumerate(paths) if p.endswith('/payload')]
                self.assertEqual(len(payload), 2 if engine == 'cross' else 1)
                destination = paths.index(str(target.parent)); removal = paths.index(str(source.parent))
                self.assertLess(max(payload), destination); self.assertLess(destination, removal)
                self.assertTrue(paths[destination+1].split('/')[-1].startswith('.openpilot-stage-'))
                self.assertTrue(all(x[1] == 1 for x in log[:removal])); self.assertEqual(log[removal][1:3], (0, 1))

    def test_interrupted_flushes_are_retried_at_each_boundary(self):
        for engine in ['native', 'cross']:
            for boundary in ['payload', 'destination', 'workspace', 'source']:
                with self.subTest(engine=engine, boundary=boundary):
                    source, target = self.case(engine)
                    pattern = {'payload': target.parent/'.openpilot-stage-*/payload',
                               'destination': target.parent, 'workspace': target.parent/'.openpilot-stage-*',
                               'source': source.parent}[boundary]
                    r, log = self.run_transfer(engine, source, target, pattern, error=errno.EINTR)
                    self.assertEqual(r['errors'], 0); self.assertFalse(source.exists()); self.assert_payload(target, 'file')
                    failures = [i for i, x in enumerate(log) if x[0] == errno.EINTR]
                    self.assertEqual(len(failures), 1); i = failures[0]
                    self.assertEqual(log[i+1], (0, *log[i][1:]))

    def test_atomic_move_retains_inode_and_flushes_namespace(self):
        for kind in ['file', 'link', 'directory']:
            with self.subTest(kind=kind):
                source, _ = self.case('native', kind); target = self.n.dst/source.name
                before = source.lstat().st_ino
                if kind == 'directory': (source/'unreadable').write_bytes(b'existing'); (source/'unreadable').chmod(0)
                elif kind == 'file': source.chmod(0)
                r, log = self.run_transfer('native', source, target)
                self.assertEqual(r['errors'], 0); self.assertFalse(os.path.lexists(source))
                self.assertEqual(target.lstat().st_ino, before)
                ancestry = [(0, 1, 0, str(p)) for p in target.parent.parents]
                self.assertEqual(log, ancestry + [(0, 0, 1, str(target.parent)), (0, 0, 1, str(source.parent))])

    def test_atomic_move_flush_failure_reports_visible_result(self):
        for boundary in ['destination', 'source']:
            with self.subTest(boundary=boundary):
                source, _ = self.case('native'); target = self.n.dst/source.name
                r, log = self.run_transfer('native', source, target,
                    target.parent if boundary == 'destination' else source.parent)
                self.failed(r, log); self.assertFalse(source.exists()); self.assert_payload(target, 'file')
                ancestry = [(0, 1, 0, str(p)) for p in target.parent.parents]
                self.assertEqual(log[:len(ancestry)], ancestry)
                self.assertEqual(len(log)-len(ancestry), 2, 'attempt both namespace flushes even if the first fails')

    def test_nested_move_persists_each_new_ancestor_before_children(self):
        for engine in ['native', 'cross']:
            with self.subTest(engine=engine):
                source, target = self.case(engine, 'directory')
                (source/'inner').mkdir(); (source/'inner/file').write_bytes(b'nested')
                (source/'inner/link').symlink_to('file')
                r, log = self.run_transfer(engine, source, target)
                self.assertEqual(r['errors'], 0); self.assertFalse(source.exists())
                self.assertEqual((target/'inner/file').read_bytes(), b'nested')
                self.assertEqual(os.readlink(target/'inner/link'), 'file')
                paths = [x[3] for x in log]
                for folder, parent in [(target, target.parent), (target/'inner', target)]:
                    i = paths.index(str(folder)); self.assertEqual(paths[i+1], str(parent))
                payload = next(i for i, p in enumerate(paths) if '/.openpilot-stage-' in p)
                self.assertLess(paths.index(str(target/'inner')), payload)
                self.assertEqual(log[-1][1:3], (0, 1))

    # Primary owner: transfer destination-version safety (engine integration).
    # Semantic inputs: linux/fpjobs.c, linux/fptransfer.c, linux/fpstage.h,
    # tests/fixtures/transfer_durability.c; run the complete owning module.
    def test_destination_changes_during_payload_flush_preserve_source(self):
        for engine in ['native', 'cross']:
            for op in [0, 1]:
                for mode in ['edit', 'metadata', 'replace', 'delete', 'link', 'directory']:
                    with self.subTest(engine=engine, op=op, mode=mode):
                        source, target = self.case(engine, replace=True)
                        target.chmod(0o640)
                        self.raced_transfer(engine, source, target, mode, op)

    def test_symlink_destination_changes_during_flush_preserve_source(self):
        for engine in ['native', 'cross']:
            for op in [0, 1]:
                for mode in ['link', 'delete', 'directory']:
                    with self.subTest(engine=engine, op=op, mode=mode):
                        source, target = self.case(engine, 'link', replace=True)
                        self.raced_transfer(engine, source, target, mode, op, 'link')

    def test_late_destination_creation_never_replaces_unexpected_file(self):
        for engine in ['native', 'cross']:
            for op in [0, 1]:
                with self.subTest(engine=engine, op=op):
                    source, target = self.case(engine)
                    self.raced_transfer(engine, source, target, 'create', op)

    def raced_transfer(self, engine, source, target, mode, op, kind='file'):
        marker = self.n.root/('mutation-'+str(self.serial))
        pattern = target.parent/('.openpilot-stage-*' if kind == 'link' else '.openpilot-stage-*/payload')
        env = dict(FP_DURABILITY_MUTATE=mode, FP_DURABILITY_MUTATE_MATCH=str(pattern),
                   FP_DURABILITY_MUTATION_LOG=str(marker))
        with patch.dict(os.environ, env), patch.dict(self.n.env, env):
            result, records = self.run_transfer(engine, source, target, op=op)
        self.assertTrue(marker.is_file(), 'mutation must occur after a real successful fsync')
        self.assertEqual((result['state'], result['errors'], result['skipped']), (3, 1, 0))
        self.assertEqual([e[1] for e in result['events']], [errno.EEXIST if mode == 'create' else errno.ESTALE])
        self.assertTrue(records)
        self.assert_payload(source, kind)
        if mode == 'delete': self.assertFalse(os.path.lexists(target))
        elif mode == 'link': self.assertEqual(os.readlink(target), 'concurrent-link')
        elif mode == 'directory': self.assertEqual(list(target.iterdir()), [])
        elif mode == 'metadata':
            self.assertEqual(target.read_bytes(), b'old target')
            self.assertEqual(target.stat().st_mode & 0o777, 0o400)
        else: self.assertEqual(target.read_bytes(), b'late edit!')
        self.assertEqual(list(target.parent.glob('.openpilot-stage-*')), [])


if __name__ == '__main__':
    unittest.main()
