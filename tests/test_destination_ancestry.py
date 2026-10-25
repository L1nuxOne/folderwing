"""Existing Linux destination ancestry, fenced before mutation/acceptance.

Private actual-writer fixtures, no power-loss claims or product fault switches.
"""
import errno
import os
from pathlib import Path
import select
import shlex
import signal
import struct
import subprocess
import tempfile
import time
import unittest
from unittest.mock import patch

import test_cross_distro as cross
import test_wsl_jobs as jobs

ROOT = Path(__file__).resolve().parents[1]


class DestinationAncestry(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix='openpilot_ancestry_build_')
        cls.addClassCleanup(cls.build.cleanup)
        cls.helper = str(Path(cls.build.name)/'ancestry-fixture')
        source = Path(os.environ.get('FP_ANCESTRY_CODE_ROOT', ROOT))
        subprocess.run([*shlex.split(os.environ.get('CC', 'cc')), '-std=c11', '-O2', '-pthread',
                        '-I'+str(ROOT/'src'), '-I'+str(source/'linux'),
                        *[str(source/'linux'/name) for name in ['fpjobs.c','fptransfer.c','fpguard.c']],
                        str(ROOT/'tests/fixtures/ancestry_durability.c'), '-Wl,--wrap=fsync', '-o', cls.helper],
                       check=True, capture_output=True)

    def setUp(self):
        self.n = jobs.NativeJobs('runTest'); self.n.setUp(); self.addCleanup(self.n.tearDown)
        self.c = cross.CrossDistro('runTest'); self.c.setUp(); self.addCleanup(self.c.tearDown)
        self.log = self.n.root/'sync.log'
        for module in [jobs, cross]:
            p = patch.object(module, 'HELPER', self.helper); p.start(); self.addCleanup(p.stop)

    def fixture(self, name):
        dest = self.c.root/name/'ancestor'/'parent'/'endpoint'; dest.mkdir(parents=True)
        source = self.c.src/name; source.write_bytes(b'complete selected bytes')
        target = dest/name; target.write_bytes(b'old target')
        return source, target

    def environment(self, **extra):
        self.log.unlink(missing_ok=True)
        return dict(FP_ANCESTRY_LOG=str(self.log), **{k:str(v) for k,v in extra.items()})

    def records(self):
        return [(int(err), path) for err,path in (line.split('\t',1) for line in self.log.read_text().splitlines())] if self.log.exists() else []

    def run_transfer(self, engine, source, target, env, move=True):
        with patch.dict(os.environ, env), patch.dict(self.n.env, env):
            return self.n.job(int(move), [source], target.parent, policy=2) if engine == 'native' else self.c.transfer(source, op=int(move), policy=2, destination=target.parent)

    def importer(self, dest, env):
        with patch.dict(os.environ, env): receiver = self.c.start(True, dest, 1, 2)
        typ,_,body = self.c.receive(receiver); self.assertEqual(typ,cross.ACK_TYPE)
        return receiver, cross.ACK.unpack(body)

    def guard(self, dest, env):
        p = subprocess.Popen([self.helper,'--guard'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,env=dict(os.environ,**env))
        self.c.children.append(p)
        self.assertEqual(self.c.receive(p)[0],1)
        self.c.send(p,43,struct.pack('<QII',0,0,0)+os.fsencode(dest))
        typ,_,data = self.c.receive(p); self.assertEqual(typ,44)
        dev,err,pid,fd,pad = struct.unpack('<QIIII',data); self.assertEqual((err,pad),(0,0))
        return p

    def guard_ancestry(self, p):
        self.c.send(p,58)
        typ,_,data = self.c.receive(p); self.assertEqual(typ,57)
        return struct.unpack('<i',data)[0]

    def test_existing_ancestors_flush_bottom_up_before_copy_or_move(self):
        for engine in ['native','cross']:
            for move in [False,True]:
                with self.subTest(engine=engine,move=move):
                    source,target = self.fixture(engine+str(move)); env = self.environment()
                    result = self.run_transfer(engine,source,target,env,move)
                    self.assertEqual(result['errors'],0)
                    parents = [str(p) for p in target.parent.parents]
                    self.assertEqual(self.records()[:len(parents)],[(0,p) for p in parents])
                    self.assertEqual(target.read_bytes(),b'complete selected bytes')
                    self.assertEqual(source.exists(),not move)

    def test_native_ancestor_errors_prevent_atomic_move_and_replacement(self):
        for error in [errno.EIO,errno.EACCES,errno.ENOTSUP]:
            for ancestor in ['parent','grandparent']:
                with self.subTest(error=error,ancestor=ancestor):
                    source,target = self.fixture(str(error)+ancestor)
                    match = target.parent.parent if ancestor=='parent' else target.parent.parent.parent
                    result = self.run_transfer('native',source,target,self.environment(FP_ANCESTRY_MATCH=match,FP_ANCESTRY_ERRNO=error))
                    self.assertEqual(result['errors'],1)
                    self.assertEqual(result['events'][0][1],error)
                    self.assertEqual(source.read_bytes(),b'complete selected bytes'); self.assertEqual(target.read_bytes(),b'old target')
                    self.assertEqual(list(target.parent.iterdir()),[target])
                    self.assertEqual(self.records()[-1],(error,str(match)))

    def test_importer_refuses_initial_acceptance_when_ancestry_fails(self):
        for error in [errno.EIO,errno.EACCES,errno.ENOTSUP]:
            with self.subTest(error=error):
                source,target = self.fixture(str(error))
                p,ack = self.importer(target.parent,self.environment(FP_ANCESTRY_MATCH=target.parent.parent,FP_ANCESTRY_ERRNO=error))
                self.assertEqual(ack,(error,0,0,0)); p.wait(timeout=5); self.assertEqual(p.returncode,0)
                self.assertEqual(source.read_bytes(),b'complete selected bytes'); self.assertEqual(target.read_bytes(),b'old target')
                self.assertEqual(list(target.parent.iterdir()),[target])

    def test_interrupted_ancestry_flush_retries_without_skipping_parent(self):
        for engine in ['native','cross']:
            with self.subTest(engine=engine):
                source,target = self.fixture(engine); match=target.parent.parent
                result = self.run_transfer(engine,source,target,self.environment(FP_ANCESTRY_MATCH=match,FP_ANCESTRY_ERRNO=errno.EINTR))
                self.assertEqual(result['errors'],0); self.assertFalse(source.exists())
                self.assertEqual(self.records()[:2],[(errno.EINTR,str(match)),(0,str(match))])

    def test_guard_uses_pinned_destination_after_name_becomes_symlink(self):
        _,target = self.fixture('guard'); original=target.parent
        p=self.guard(original,self.environment())
        held=original.with_name('held'); original.rename(held); original.symlink_to('/')
        self.assertEqual(self.guard_ancestry(p),0)
        parents=[str(v) for v in held.parents]
        self.assertEqual(self.records(),[(0,v) for v in parents])
        self.assertEqual((held/target.name).read_bytes(),b'old target')

    def test_mount_root_stops_before_unrelated_parent_filesystem(self):
        with tempfile.TemporaryDirectory(prefix='openpilot_ancestry_',dir='/dev/shm') as name:
            endpoint=Path(name)/'deep'; endpoint.mkdir()
            self.assertNotEqual(endpoint.stat().st_dev,Path('/dev').stat().st_dev)
            p=self.guard(endpoint,self.environment())
            self.assertEqual(self.guard_ancestry(p),0)
            self.assertEqual(self.records(),[(0,name),(0,'/dev/shm')])

    def test_endpoint_reparented_during_barrier_is_rejected(self):
        for engine in ['native','cross']:
            with self.subTest(engine=engine):
                source,target=self.fixture(engine); moved=self.c.root/('moved-'+engine)
                env=self.environment(FP_ANCESTRY_MATCH=target.parent.parent,FP_ANCESTRY_MOVE=target.parent,FP_ANCESTRY_TO=moved)
                if engine=='native':
                    result=self.run_transfer(engine,source,target,env)
                    self.assertEqual(result['events'][0][1],errno.ESTALE)
                else:
                    p,ack=self.importer(target.parent,env); self.assertEqual(ack,(errno.ESTALE,0,0,0));p.wait(timeout=5)
                self.assertEqual(source.read_bytes(),b'complete selected bytes')
                self.assertEqual((moved/target.name).read_bytes(),b'old target')
                self.assertEqual(list(moved.iterdir()),[moved/target.name])

    def test_cancellation_during_ancestry_flush_preserves_source_and_target(self):
        source,target=self.fixture('cancel')
        env=self.environment(FP_ANCESTRY_MATCH=target.parent.parent,FP_ANCESTRY_STOP=1)
        p=subprocess.Popen([self.helper,'--jobs'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,env=dict(os.environ,**env))
        self.c.children.append(p);self.assertEqual(self.c.receive(p)[0],1)
        body=struct.pack('<IIII',1,2,1,0)
        for path in [target.parent,source]:raw=os.fsencode(path);body+=struct.pack('<II',len(raw),0)+raw
        self.c.send(p,10,body)
        while True:
            typ,_,data=self.c.receive(p);self.assertEqual(typ,11)
            if cross.PROGRESS.unpack(data[:48])[2]==2:break
        self.c.send(p,14)
        deadline=time.monotonic()+5
        while time.monotonic()<deadline:
            pid,status=os.waitpid(p.pid,os.WNOHANG|os.WUNTRACED)
            if pid:
                self.assertTrue(os.WIFSTOPPED(status));break
            time.sleep(.01)
        else:self.fail('private writer never reached ancestry barrier')
        self.c.send(p,13);os.kill(p.pid,signal.SIGCONT)
        while True:
            typ,_,data=self.c.receive(p);self.assertEqual(typ,11)
            state=cross.PROGRESS.unpack(data[:48])[2]
            if state>=3:break
        self.assertEqual(state,4);p.wait(timeout=5)
        self.assertEqual(source.read_bytes(),b'complete selected bytes');self.assertEqual(target.read_bytes(),b'old target')

    def test_selected_regular_leaf_uses_previously_fenced_root(self):
        for engine in ['native','cross']:
            with self.subTest(engine=engine):
                source,target=self.fixture(engine)
                guard=self.guard(target.parent,self.environment())
                self.assertEqual(self.guard_ancestry(guard),0)
                env=self.environment(FP_ANCESTRY_MATCH=target.parent.parent,FP_ANCESTRY_ERRNO=errno.EIO)
                with patch.dict(os.environ,env),patch.dict(self.n.env,env):
                    result=self.n.job(9,[source],target.parent,policy=2) if engine=='native' else self.c.transfer(source,op=9,policy=2,destination=target.parent)
                self.assertEqual(result['errors'],0);self.assertEqual(source.read_bytes(),b'complete selected bytes')
                self.assertEqual(target.read_bytes(),b'complete selected bytes')
                self.assertNotIn(str(target.parent.parent),[path for _,path in self.records()])

    def test_bind_mount_root_does_not_flush_host_ancestors(self):
        # A same-device bind mount defeats st_dev-only boundary detection.
        # Own a disposable mount namespace; do not mount anything in the user
        # namespace of the app/distro or require elevation.
        script=r'''
import ctypes,os,struct,subprocess,sys,tempfile
from pathlib import Path
helper=sys.argv[1]
with tempfile.TemporaryDirectory(prefix='openpilot_ancestry_mount_') as name:
 root=Path(name);actual=root/'actual';mount=root/'mounted';actual.mkdir();mount.mkdir();(actual/'deep').mkdir()
 subprocess.run(['mount','--bind',str(actual),str(mount)],check=True,capture_output=True)
 try:
  log=root/'log'
  p=subprocess.Popen([helper,'--guard'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,env=dict(os.environ,FP_ANCESTRY_LOG=str(log)))
  try:
   def read(n):
    data=b''
    while len(data)<n:
     part=p.stdout.read(n-len(data));assert part;data+=part
    return data
   assert struct.unpack('<III',read(12))==(1,16,0);read(16)
   data=struct.pack('<QII',0,0,0)+os.fsencode(mount/'deep')
   p.stdin.write(struct.pack('<III',43,len(data),1)+data);p.stdin.flush()
   assert struct.unpack('<III',read(12))==(44,24,1);assert struct.unpack('<QIIII',read(24))[1]==0
   p.stdin.write(struct.pack('<III',58,0,1));p.stdin.flush()
   assert struct.unpack('<III',read(12))==(57,4,1);assert struct.unpack('<i',read(4))==(0,)
   assert log.read_text().splitlines()==['0\t'+str(mount)],log.read_text()
  finally:
   p.stdin.close();p.wait(timeout=5);p.stdout.close()
 finally:subprocess.run(['umount',str(mount)],check=True,capture_output=True)
'''
        result=subprocess.run(['unshare','--user','--map-root-user','--mount','--propagation','private',
            os.sys.executable,'-c',script,self.helper],capture_output=True,timeout=15)
        self.assertEqual(result.returncode,0,result.stderr.decode(errors='replace'))


if __name__=='__main__':unittest.main()
