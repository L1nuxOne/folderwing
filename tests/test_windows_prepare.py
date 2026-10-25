"""Copy-only Windows staging-credit protocol and held Linux source contracts.
The fake coordinator is not an NTFS or crash-persistence oracle.
"""
import errno
import os
from pathlib import Path
import select
import resource
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
HEAD = struct.Struct('<III')
PROGRESS = struct.Struct('<QQIIIIIiII')
RESULT = struct.Struct('<QiI')
SOURCES = ['linux/fpjobs.c', 'linux/fpwinprepare.h', 'linux/fpwingroup.h', 'linux/fpmetadata.h',
           'src/wsl.c', 'src/winprepare.h', 'src/wingroup.h', 'src/wslproto.h',
           'tests/fixtures/windows_prepare.c', 'tests/test_windows_prepare.py']
SELECTORS = ['test_credits_are_not_commits_and_width_is_bounded',
             'test_source_versions_and_late_metadata_rechecked_before_commit',
             'test_commit_errors_abort_later_stages_without_source_removal',
             'test_cancellation_between_credit_and_commit_drains',
             'test_wrong_tokens_failed_credits_and_disconnects_fail_closed',
             'test_conflict_policies_duplicates_and_mixed_serial_fallback',
             'test_owned_process_interruption_preserves_all_sources',
             'test_invalid_queue_requests_are_refused',
             'test_group_credits_wait_for_correlated_durable_results',
             'test_group_fresh_source_checks_refuse_late_changes',
             'test_group_partial_errors_and_cancellation_retain_sources',
             'test_group_malformed_checks_and_results_fail_closed']
TEST_CONTRACTS = {n: dict(primary='Linux integration',
    owner='Windows preparation credits, retained Linux sources and ordered durable results',
    sources=SOURCES) for n in SELECTORS}

for selector in SELECTORS:
    if selector.startswith('test_group_'):
        TEST_CONTRACTS[selector]['owner']='Windows group source validation, correlated durable credit and fail-closed protocol'

class WindowsPrepare(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix='openpilot_win_prepare_build_')
        cls.addClassCleanup(cls.build.cleanup)
        cls.helper = str(Path(cls.build.name)/'helper')
        subprocess.run(['cc', '-std=c11', '-O2', '-pthread', '-I'+str(ROOT/'src'),
                        str(ROOT/'tests/fixtures/windows_prepare.c'), '-o', cls.helper],check=True,capture_output=True)

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='openpilot_win_prepare_')
        self.addCleanup(self.tmp.cleanup);self.root=Path(self.tmp.name)
        self.src=self.root/'source';self.dst=self.root/'destination';self.src.mkdir();self.dst.mkdir()
        self.files=[]

    def file(self,name=None,size=4096):
        p=self.src/(name or f'file-{len(self.files):04d}');p.parent.mkdir(exist_ok=True)
        p.write_bytes(bytes([len(self.files)%256])*size);self.files.append(p);return p

    def run_job(self, sources, width=4, op=0, policy=0, mutate=None, fail_credit=0,
                fail_commit=0, wrong=None, cancel_credit=0, interrupt=None, windows=True, fd_limit=None, group=False):
        p=subprocess.Popen([self.helper],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,
                           preexec_fn=(lambda:resource.setrlimit(resource.RLIMIT_NOFILE,(fd_limit,fd_limit))) if fd_limit else None)
        self.addCleanup(lambda:p.poll() is None and (p.kill(),p.wait()))
        def read(n):
            out=b''
            while len(out)<n:
                if not select.select([p.stdout],[],[],10)[0]:raise AssertionError('protocol timeout')
                part=os.read(p.stdout.fileno(),n-len(out))
                if not part:raise EOFError
                out+=part
            return out
        def receive():
            kind,n,ident=HEAD.unpack(read(12));self.assertLessEqual(n,1<<20);return kind,ident,read(n)
        def send(kind,b=b'',ident=1):
            p.stdin.write(HEAD.pack(kind,len(b),ident)+b);p.stdin.flush()
        out=dict(credits=0,commits=0,peak=0,serial=0,events=[],commands=[],progress=[],published=[],interrupted=False)
        pending=[];active=None
        try:
            kind,_,hello=receive();self.assertEqual(kind,1);self.assertEqual(struct.unpack('<4I',hello)[1],8)
            body=struct.pack('<4I',op,policy,len(sources),width | (256 if group else 0))
            for i,path in enumerate([self.dst,*sources]):
                raw=os.fsencode(path);body+=struct.pack('<II',len(raw),int(i==0 and windows))+raw
            send(10,body)
            while True:
                try:kind,ident,body=receive()
                except EOFError:
                    p.wait(timeout=5);out['eof']=True;return out
                self.assertEqual(ident,1)
                if mutate:mutate(kind,body,out)
                if kind==11:
                    v=PROGRESS.unpack(body[:48]);out['progress'].append(v)
                    if v[2]==2 and not out.get('continued'):send(14);out['continued']=True
                    if v[2]>=3:out.update(done=v[0],items=v[3],errors=v[5],state=v[2]);p.wait(timeout=5);self.assertEqual(p.returncode,0);return out
                elif kind==12:
                    out['events'].append(struct.unpack('<IiII',body[:16])[1])
                elif kind==54:
                    self.assertEqual(pending,[]);send(55,struct.pack('<i',0))
                elif kind in [15,61]:
                    self.assertIsNone(active)
                    at,mt,replace,root,n,pad=struct.unpack('<qqIIII',body[:32]);self.assertEqual((root,pad),(0,0))
                    offset=48 if kind==61 else 32
                    token,size=struct.unpack('<QQ',body[32:48]) if kind==61 else (0,None)
                    self.assertEqual(n,len(body)-offset);name=os.fsdecode(body[offset:]).lstrip('/')
                    active=dict(token=token,size=size,bytes=b'',name=name,mtime=mt,replace=replace)
                    if kind==15:out['serial']+=1;self.assertEqual(pending,[])
                elif kind==16:
                    self.assertIsNotNone(active);self.assertLessEqual(len(body),65536);active['bytes']+=body
                elif kind==17:
                    error=struct.unpack('<i',body)[0]
                    if active['token']:
                        self.assertEqual(len(active['bytes']),active['size'])
                        out['credits']+=1
                        if fail_credit==out['credits']:error=errno.ENOSPC
                        if not error:pending.append(active);out['peak']=max(out['peak'],len(pending))
                        self.assertLessEqual(len(pending),width)
                        if cancel_credit==out['credits']:send(13)
                        token=active['token']+(1 if wrong=='credit' else 0)
                        if interrupt=='prepared':p.kill();p.wait();out['interrupted']=True;return out
                        send(62,RESULT.pack(token,error,0),2 if wrong=='id' else 1)
                    else:
                        if not error:self.publish(active,out)
                        send(18,struct.pack('<i',error))
                    active=None
                elif kind==65:
                    self.assertTrue(group);self.assertIsNone(active)
                    count,pad=struct.unpack('<II',body[:8]);self.assertEqual((count,pad),(len(pending),0))
                    self.assertEqual(len(body),8+count*16)
                    commands=[RESULT.unpack_from(body,8+i*16) for i in range(count)]
                    self.assertEqual([v[0] for v in commands],[v['token'] for v in pending])
                    self.assertTrue(all(v[2]==0 for v in commands))
                    results=[];error=next((v[1] for v in commands if v[1]),0)
                    if wrong=='early-success':
                        send(68,struct.pack('<II',count,0)+b''.join(RESULT.pack(v[0],0,0) for v in commands));pending.clear();continue
                    for current in pending:
                        out['commands'].append(error);out['commits']+=1
                        if not error:
                            token=current['token']+(1 if wrong=='check-token' else 0)
                            send(66,RESULT.pack(token,0,1 if wrong=='check-pad' else 0),2 if wrong=='check-id' else 1)
                            try:kind,ident,answer=receive()
                            except EOFError:p.wait(timeout=5);out['eof']=True;return out
                            if wrong in ['check-token','check-pad','check-id']:
                                self.assertEqual((kind,ident),(11,1));v=PROGRESS.unpack(answer[:48])
                                self.assertEqual((v[0],v[2]),(0,4));p.wait(timeout=5)
                                out.update(done=v[0],items=v[3],errors=v[5],state=v[2]);return out
                            self.assertEqual((kind,ident),(67,1))
                            checked, error, pad=RESULT.unpack(answer);self.assertEqual((checked,pad),(current['token'],0))
                            if fail_commit==out['commits']:error=errno.EIO
                        if not error:self.publish(current,out)
                        results.append((current['token'],error,0))
                    if wrong=='disconnect':p.stdin.close();p.kill();p.wait();out['eof']=True;return out
                    if wrong=='result-token':results[-1]=(results[0][0],results[-1][1],0)
                    if wrong=='result-error':results[-1]=(results[-1][0],4096,0)
                    reply=struct.pack('<II',count+(1 if wrong=='result-count' else 0),int(wrong=='result-pad'))+b''.join(RESULT.pack(*v) for v in results)
                    send(68,reply,2 if wrong=='result-id' else 1);pending.clear()
                elif kind==63:
                    token,error,pad=RESULT.unpack(body);self.assertEqual(pad,0);self.assertTrue(pending)
                    current=pending.pop(0);self.assertEqual(token,current['token']);out['commands'].append(error);out['commits']+=1
                    if fail_commit==out['commits']:error=errno.EIO
                    if not error:self.publish(current,out)
                    if interrupt=='committed':p.kill();p.wait();out['interrupted']=True;return out
                    if wrong=='disconnect':p.stdin.close();p.kill();p.wait();out['eof']=True;return out
                    send(64,RESULT.pack(token+(1 if wrong=='commit' else 0),error,0))
                else:self.fail(f'unexpected message {kind}')
        finally:
            if p.poll() is None:p.kill();p.wait()
            for stream in [p.stdin,p.stdout,p.stderr]:
                if not stream.closed:stream.close()

    def publish(self,current,out):
        target=self.dst/current['name'];target.parent.mkdir(parents=True,exist_ok=True)
        if target.exists() and not current['replace']:self.fail('fake host exclusive publication collision')
        target.write_bytes(current['bytes']);os.utime(target,ns=(current['mtime'],current['mtime']))
        out['published'].append(current['name'])

    def test_credits_are_not_commits_and_width_is_bounded(self):
        files=[self.file() for _ in range(25)]
        for width in [2,4,8]:
            for p in self.dst.iterdir():p.unlink()
            def inspect(kind,body,out):
                if kind==63 and out['commits']==0:
                    self.assertEqual(out['credits'],width)
                    self.assertEqual(out['progress'][-1][0:1],(0,));self.assertEqual(out['progress'][-1][3],0)
            r=self.run_job(files,width=width,mutate=inspect)
            self.assertEqual((r['errors'],r['done'],r['items'],r['peak']),(0,25*4096,25,width))
            self.assertEqual((r['credits'],r['commits'],r['serial']),(25,25,0))
            for f in files:self.assertEqual((self.dst/f.name).read_bytes(),f.read_bytes())
        many=[self.file() for _ in range(250)]
        r=self.run_job(many,width=8,fd_limit=96);self.assertEqual((r['errors'],r['credits'],r['commits']),(0,250,250))
        r=self.run_job([self.file() for _ in range(30)],width=8,fd_limit=12)
        self.assertGreater(r['errors'],0);self.assertEqual(r['done'],0)

    def test_source_versions_and_late_metadata_rechecked_before_commit(self):
        for change in ['rewrite','replace','chmod','xattr']:
            files=[self.file() for _ in range(4)];changed=False
            def mutate(kind,body,out):
                nonlocal changed
                if kind==17 and out['credits']==3 and not changed:
                    if change=='rewrite':files[0].write_bytes(b'changed')
                    elif change=='replace':files[0].rename(files[0].with_suffix('.saved'));files[0].write_bytes(b'foreign')
                    elif change=='chmod':files[0].chmod(0o600)
                    else:os.setxattr(files[0],'user.late',b'late')
                    changed=True
            r=self.run_job(files,mutate=mutate)
            self.assertTrue(changed);self.assertEqual(r['done'],0);self.assertGreater(r['errors'],0)
            self.assertEqual(r['commands'][0],errno.ESTALE if change!='xattr' else errno.ESTALE)
            self.assertTrue(all(f.exists() for f in files));self.assertEqual(r['published'],[])

    def test_commit_errors_abort_later_stages_without_source_removal(self):
        files=[self.file() for _ in range(8)]
        r=self.run_job(files,fail_commit=2)
        self.assertGreater(r['errors'],0);self.assertEqual((r['done'],r['published']),(4096,[files[0].name]))
        self.assertEqual(r['commits'],4);self.assertTrue(all(f.exists() for f in files))

    def test_cancellation_between_credit_and_commit_drains(self):
        files=[self.file() for _ in range(8)]
        for at in [1,4]:
            r=self.run_job(files,cancel_credit=at)
            self.assertEqual((r['state'],r['done'],r['published']),(4,0,[]))
            self.assertEqual(r['credits'],r['commits']);self.assertTrue(all(f.exists() for f in files))

    def test_wrong_tokens_failed_credits_and_disconnects_fail_closed(self):
        files=[self.file() for _ in range(8)]
        for wrong in ['credit','id','commit','disconnect']:
            r=self.run_job(files,wrong=wrong)
            self.assertTrue(r.get('eof') or r['state']==4)
            self.assertTrue(all(f.exists() for f in files))
            for p in self.dst.iterdir():p.unlink()
        r=self.run_job(files,fail_credit=2);self.assertEqual(r['done'],0);self.assertGreater(r['errors'],0)

    def test_conflict_policies_duplicates_and_mixed_serial_fallback(self):
        one=self.file('same');two=self.file('elsewhere/same')
        r=self.run_job([one,two,one]);self.assertEqual(r['errors'],0)
        self.assertEqual(sorted(p.name for p in self.dst.iterdir()),['same','same (2)','same (3)'])
        for p in self.dst.iterdir():p.unlink()
        files=[self.file(),self.file('empty',0),self.file('large',1048577),self.file('unicode-λ'),self.file()]
        r=self.run_job(files);self.assertEqual(r['errors'],0);self.assertEqual((r['credits'],r['serial']),(2,3))
        r=self.run_job(files,policy=1);self.assertEqual((r['errors'],r['credits'],r['serial']),(0,0,0))
        r=self.run_job(files,policy=2);self.assertEqual((r['errors'],r['credits'],r['serial']),(0,0,5))
        # Moves and selected-file operations cannot negotiate preparation credits.
        for op in [1,9]:
            r=self.run_job([one],width=0,op=op,policy=1);self.assertEqual(r['credits'],0)

    def test_owned_process_interruption_preserves_all_sources(self):
        files=[self.file() for _ in range(8)]
        for point in ['prepared','committed']:
            r=self.run_job(files,interrupt=point);self.assertTrue(r['interrupted']);self.assertTrue(all(f.exists() for f in files))
            self.assertEqual(len(r['published']),int(point=='committed'))
            for p in self.dst.iterdir():p.unlink()

    def test_invalid_queue_requests_are_refused(self):
        f=self.file()
        for width,op,windows in [(1,0,True),(9,0,True),(2,1,True),(2,9,True),(2,0,False),(256,0,True),(257,0,True),(265,0,True),(512|4,0,True),(260,1,True),(260,0,False)]:
            r=self.run_job([f],width=width,op=op,windows=windows)
            self.assertEqual(r['credits'],0);self.assertTrue(r.get('eof') or r['state']==5)

    def test_group_credits_wait_for_correlated_durable_results(self):
        for width in [2,4,8]:
            with self.subTest(width=width):
                files=[self.file() for _ in range(width+2)]
                def inspect(kind,body,out):
                    if kind==65 and not out['commits']:
                        self.assertEqual(out['credits'],width)
                        self.assertEqual((out['progress'][-1][0],out['progress'][-1][3]),(0,0))
                r=self.run_job(files,width=width,group=True,mutate=inspect)
                self.assertEqual((r['state'],r['errors'],r['done'],r['items'],r['peak']),(3,0,len(files)*4096,len(files),width))
                for f in files:self.assertEqual((self.dst/f.name).read_bytes(),f.read_bytes())

    def test_group_fresh_source_checks_refuse_late_changes(self):
        for change in ['rewrite','replace','chmod','xattr']:
            files=[self.file() for _ in range(4)];changed=False
            def mutate(kind,body,out):
                nonlocal changed
                if kind==65 and not changed:
                    if change=='rewrite':files[0].write_bytes(b'late')
                    elif change=='replace':files[0].rename(files[0].with_suffix('.saved'));files[0].write_bytes(b'foreign')
                    elif change=='chmod':files[0].chmod(0o600)
                    else:os.setxattr(files[0],'user.late',b'late')
                    changed=True
            r=self.run_job(files,group=True,mutate=mutate)
            self.assertTrue(changed);self.assertGreater(r['errors'],0)
            self.assertEqual((r['done'],r['published']),(0,[]));self.assertTrue(all(f.exists() for f in files))

    def test_group_partial_errors_and_cancellation_retain_sources(self):
        for width in [4,8]:
            files=[self.file() for _ in range(width+2)]
            r=self.run_job(files,width=width,group=True,fail_commit=2)
            self.assertGreater(r['errors'],0);self.assertEqual((r['done'],r['published']),(4096,[files[0].name]))
            self.assertTrue(all(f.exists() for f in files))
            for p in self.dst.iterdir():p.unlink()
            r=self.run_job(files,width=width,group=True,cancel_credit=width)
            self.assertEqual((r['state'],r['done'],r['published']),(4,0,[]));self.assertTrue(all(f.exists() for f in files))

    def test_group_malformed_checks_and_results_fail_closed(self):
        files=[self.file() for _ in range(4)]
        for wrong in ['check-token','check-pad','check-id','early-success','result-token','result-error','result-count','result-pad','result-id','disconnect']:
            with self.subTest(wrong=wrong):
                r=self.run_job(files,group=True,wrong=wrong)
                self.assertTrue(r.get('eof') or r['state']==4)
                if not r.get('eof'):self.assertEqual(r['done'],0)
                self.assertTrue(all(f.exists() for f in files))
                for p in self.dst.iterdir():p.unlink()

if __name__=='__main__':unittest.main()
