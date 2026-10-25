"""Engine integration: Linux ACL/xattr fidelity, inherited-policy removal,
verified persistence and failure retention. Owners: linux/fpmetadata.h,
linux/fpjobs.c, linux/fptransfer.c and the required wslp_node metadata contract.
All fixtures are owned temporary trees; no global CPU/VM budgets apply.
"""
import contextlib
import errno
import hashlib
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import test_wsl_jobs as jobs
import test_cross_distro as cross

HELPER = sys.argv.pop(1) if __name__ == '__main__' and len(sys.argv)>1 and not sys.argv[1].startswith('-') else jobs.HELPER
ROOT = Path(__file__).resolve().parents[1]
ACCESS = 'system.posix_acl_access'
DEFAULT = 'system.posix_acl_default'
USER = 'user.openpilot.audit'

def acl(owner=6, named=4, group=0, mask=4, other=0):
    return struct.pack('<I', 2)+b''.join(struct.pack('<HHI', tag, perm, uid) for tag,perm,uid in
        [(1,owner,0xffffffff),(2,named,65534),(4,group,0xffffffff),(16,mask,0xffffffff),(32,other,0xffffffff)])

def attributes(path):
    return {name:os.getxattr(path,name,follow_symlinks=False) for name in os.listxattr(path,follow_symlinks=False)}

def record(name,value):
    raw=os.fsencode(name)+b'\0';return struct.pack('<II',len(raw),len(value))+raw+value

class MetadataFixtures:
    def __init__(self, helper):
        self.helper=helper;self.n=jobs.NativeJobs('runTest');self.n.setUp();self.c=cross.CrossDistro('runTest');self.c.setUp()
        self.extra=tempfile.TemporaryDirectory(prefix='openpilot_metadata_dest_',dir='/dev/shm');self.dst=Path(self.extra.name)
        self.n.dst=self.c.dst=self.dst;self.src=self.n.src;self.serial=0
    def close(self):
        self.c.tearDown();self.n.tearDown();self.extra.cleanup()
    def transfer(self,engine,source,op=0,policy=2,env=None,mutate=None,helper=None):
        with contextlib.ExitStack() as stack:
            stack.enter_context(patch.object(jobs,'HELPER',helper or self.helper));stack.enter_context(patch.object(cross,'HELPER',helper or self.helper))
            stack.enter_context(patch.dict(os.environ,env or {}));stack.enter_context(patch.dict(self.n.env,env or {}))
            return self.n.job(op,[source],self.dst,policy=policy,mutate=mutate) if engine=='native' else self.c.transfer(source,op=op,policy=policy,mutate=mutate,destination=self.dst)
    def file(self):
        self.serial+=1;p=self.src/('node-'+str(self.serial));p.write_bytes(b'complete bytes');return p

class LinuxMetadata(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build=tempfile.TemporaryDirectory(prefix='openpilot_metadata_build_');cls.addClassCleanup(cls.build.cleanup)
        cls.fault=str(Path(cls.build.name)/'metadata-fixture')
        subprocess.run(['cc','-std=c11','-O2','-pthread','-I'+str(ROOT/'src'),str(ROOT/'linux/fpjobs.c'),str(ROOT/'linux/fptransfer.c'),str(ROOT/'tests/fixtures/metadata.c'),
            *['-Wl,--wrap='+n for n in ['flistxattr','fgetxattr','fsetxattr','fremovexattr','fsync']],'-o',cls.fault],check=True,capture_output=True)
    def setUp(self):
        self.f=MetadataFixtures(HELPER);self.addCleanup(self.f.close)
    def test_regular_copy_move_and_selected_copy_preserve_acl_and_binary_xattrs(self):
        for engine in ['native','cross']:
            for op in [0,1,9]:
                with self.subTest(engine=engine,op=op):
                    source=self.f.file();os.setxattr(source,USER,b'binary\0metadata\xff');os.setxattr(source,'user.empty',b'');os.setxattr(source,b'user.raw_\xff',b'raw name');os.setxattr(source,ACCESS,acl())
                    before=attributes(source);result=self.f.transfer(engine,source,op=op);target=self.f.dst/source.name
                    self.assertEqual((result['state'],result['errors']),(3,0));self.assertEqual(attributes(target),before)
                    self.assertEqual(target.read_bytes(),b'complete bytes');self.assertEqual(target.stat().st_mode&0o777,0o640);self.assertEqual(source.exists(),op!=1)
    def test_absent_source_acls_remove_inherited_access_and_default_acls(self):
        os.setxattr(self.f.dst,DEFAULT,acl(owner=7,named=7,mask=7))
        for engine in ['native','cross']:
            for directory in [False,True]:
                source=self.f.file()
                if directory:source.unlink();source.mkdir();(source/'child').write_bytes(b'private');(source/'child').chmod(0o640)
                source.chmod(0o750 if directory else 0o640)
                result=self.f.transfer(engine,source,op=1);target=self.f.dst/source.name
                self.assertEqual(result['errors'],0);self.assertEqual(attributes(target),{})
                if directory:self.assertEqual(attributes(target/'child'),{})
    def test_directory_access_default_and_user_metadata_survive_copy_and_move(self):
        for engine in ['native','cross']:
            for op in [0,1]:
                source=self.f.file();source.unlink();source.mkdir();(source/'child').write_bytes(b'child')
                os.setxattr(source,USER,b'directory metadata');os.setxattr(source,ACCESS,acl(owner=7,named=5,mask=5));os.setxattr(source,DEFAULT,acl(owner=7,named=4,mask=4))
                before=attributes(source);result=self.f.transfer(engine,source,op=op);target=self.f.dst/source.name
                self.assertEqual(result['errors'],0);self.assertEqual(attributes(target),before);self.assertEqual((target/'child').read_bytes(),b'child');self.assertEqual(source.exists(),op!=1)
    def test_replacements_drop_old_target_metadata_after_success(self):
        for engine in ['native','cross']:
            source=self.f.file();os.setxattr(source,USER,b'new');target=self.f.dst/source.name;target.write_bytes(b'old');os.setxattr(target,'user.target-only',b'old metadata');os.setxattr(target,ACCESS,acl())
            result=self.f.transfer(engine,source);self.assertEqual(result['errors'],0);self.assertEqual(attributes(target),{USER:b'new'})
    def test_directory_merges_refuse_incompatible_source_metadata_before_children(self):
        for engine in ['native','cross']:
            source=self.f.file();source.unlink();source.mkdir();(source/'child').write_bytes(b'retain');os.setxattr(source,USER,b'source directory')
            target=self.f.dst/source.name;target.mkdir();os.setxattr(target,USER,b'existing directory')
            result=self.f.transfer(engine,source,op=1)
            self.assertEqual(result['errors'],1);self.assertEqual(result['events'][0][1],errno.ENOTSUP);self.assertEqual((source/'child').read_bytes(),b'retain');self.assertEqual(list(target.iterdir()),[]);self.assertEqual(os.getxattr(target,USER),b'existing directory')
    def test_metadata_failures_and_failed_readback_never_replace_targets(self):
        for engine in ['native','cross']:
            for operation,error in [('list',errno.EACCES),('get',errno.EIO),('set',errno.ENOTSUP),('drop',errno.EIO),('remove',errno.EACCES)]:
                with self.subTest(engine=engine,operation=operation):
                    source=self.f.file();os.setxattr(source,USER,b'complete metadata');target=self.f.dst/source.name;target.write_bytes(b'old');os.setxattr(target,'user.target-only',b'old metadata');old=attributes(target)
                    if operation=='remove':os.setxattr(self.f.dst,DEFAULT,acl(owner=7,named=4,mask=4))
                    match=str(source) if operation in ['list','get'] else str(self.f.dst/'.openpilot-stage-*'/'payload')
                    env={'FP_META_FAIL':operation,'FP_META_MATCH':match,'FP_META_ERRNO':str(error)}
                    result=self.f.transfer(engine,source,op=1,env=env,helper=self.fault)
                    self.assertEqual(result['errors'],1);self.assertEqual(result['events'][0][1],error);self.assertEqual(source.read_bytes(),b'complete bytes');self.assertEqual(target.read_bytes(),b'old');self.assertEqual(attributes(target),old);self.assertEqual(list(self.f.dst.glob('.openpilot-stage-*')),[])
    def test_metadata_is_verified_before_payload_flush_and_source_removal(self):
        for engine in ['native','cross']:
            source=self.f.file();os.setxattr(source,USER,b'persisted metadata');target=self.f.dst/source.name;target.write_bytes(b'old')
            log=self.f.n.root/('sync-'+engine);result=self.f.transfer(engine,source,op=1,helper=self.fault,env={'FP_META_SYNC_LOG':str(log),'FP_META_SOURCE':str(source),'FP_META_TARGET':str(target)})
            self.assertEqual(result['errors'],0);records=[line.split('\t',4) for line in log.read_text().splitlines()]
            complete=[r for r in records if r[0]=='1' and r[3]==str(len(b'persisted metadata'))];self.assertTrue(complete);self.assertEqual(complete[0][1:3],['1','1']);self.assertEqual(os.getxattr(target,USER),b'persisted metadata');self.assertFalse(source.exists())
    def test_source_metadata_mutation_after_payload_flush_refuses_publication(self):
        for engine in ['native','cross']:
            source=self.f.file();os.setxattr(source,USER,b'original');target=self.f.dst/source.name;target.write_bytes(b'old')
            result=self.f.transfer(engine,source,op=1,helper=self.fault,env={'FP_META_MUTATE':'change','FP_META_SOURCE':str(source),'FP_META_MUTATE_MATCH':str(self.f.dst/'.openpilot-stage-*'/'payload')})
            self.assertEqual(result['errors'],1);self.assertEqual(result['events'][0][1],errno.ESTALE);self.assertEqual(os.getxattr(source,USER),b'late metadata');self.assertEqual(target.read_bytes(),b'old' if engine=='native' else b'complete bytes')
            if engine=='cross':self.assertEqual(os.getxattr(target,USER),b'original')
    def test_importer_rejects_malformed_and_unsupported_metadata_before_mutation(self):
        active=patch.object(cross,'HELPER',self.f.helper);active.start();self.addCleanup(active.stop)
        p=self.f.c.start(True,self.f.dst);self.assertEqual(cross.ACK.unpack(self.f.c.receive(p)[2])[0],0)
        valid=record(USER,b'value')
        for raw in [b'x',record('security.capability',b'unsupported'),valid+valid,record(DEFAULT,acl()),struct.pack('<II',99,0)+b'a\0',record(ACCESS,b'bad'),record(ACCESS,struct.pack('<I',2)+b''.join(struct.pack('<HHI',tag,perm,uid) for tag,perm,uid in [(1,6,0xffffffff),(2,4,65534),(4,0,0xffffffff),(32,0,0xffffffff)]))]:
            name=b'candidate';body=cross.NODE.pack(0,0,0,0,0,0,0o100640,os.getuid(),os.getgid(),len(name),0,len(raw))+name+raw
            self.f.c.send(p,cross.NODE_TYPE,body);typ,_,reply=self.f.c.receive(p);self.assertEqual(typ,cross.ACK_TYPE);self.assertNotEqual(cross.ACK.unpack(reply)[0],0);self.assertFalse((self.f.dst/'candidate').exists())
        self.f.c.send(p,cross.QUIT);p.wait(timeout=5)
    def test_merge_destination_metadata_change_retains_source_directory(self):
        for engine in ['native','cross']:
            source=self.f.file();source.unlink();source.mkdir();(source/'child').write_bytes(b'complete child');target=self.f.dst/source.name;target.mkdir()
            for node in [source,target]:os.setxattr(node,USER,b'matching directory')
            result=self.f.transfer(engine,source,op=1,helper=self.fault,env={'FP_META_MUTATE':'change','FP_META_SOURCE':str(target),'FP_META_MUTATE_MATCH':str(target/'.openpilot-stage-*'/'payload')})
            self.assertEqual(result['errors'],1);self.assertEqual(result['events'][0][1],errno.EIO);self.assertTrue(source.is_dir());self.assertEqual(list(source.iterdir()),[])
            self.assertEqual(os.getxattr(target,USER),b'late metadata');self.assertEqual(os.getxattr(source,USER),b'matching directory');self.assertEqual((target/'child').read_bytes(),b'complete child')
    def test_directory_metadata_change_during_children_retains_source_directory(self):
        for engine in ['native','cross']:
            source=self.f.file();source.unlink();source.mkdir();(source/'child').write_bytes(b'complete child');os.setxattr(source,USER,b'original directory');target=self.f.dst/source.name
            result=self.f.transfer(engine,source,op=1,helper=self.fault,env={'FP_META_MUTATE':'change','FP_META_SOURCE':str(source),'FP_META_MUTATE_MATCH':str(target/'.openpilot-stage-*'/'payload')})
            self.assertEqual(result['errors'],1);self.assertEqual(result['events'][0][1],errno.ESTALE);self.assertTrue(source.is_dir());self.assertEqual(os.getxattr(source,USER),b'late metadata');self.assertEqual(os.getxattr(target,USER),b'original directory');self.assertEqual((target/'child').read_bytes(),b'complete child')
    def test_default_acl_failure_precedes_child_mutation(self):
        for engine in ['native','cross']:
            source=self.f.file();source.unlink();source.mkdir();(source/'child').write_bytes(b'retain child');os.setxattr(source,DEFAULT,acl(owner=7,named=4,mask=4));target=self.f.dst/source.name
            result=self.f.transfer(engine,source,op=1,helper=self.fault,env={'FP_META_FAIL':'set','FP_META_MATCH':str(target),'FP_META_NAME':DEFAULT,'FP_META_ERRNO':str(errno.ENOTSUP)})
            self.assertEqual(result['errors'],1);self.assertEqual(result['events'][0][1],errno.ENOTSUP);self.assertEqual((source/'child').read_bytes(),b'retain child');self.assertEqual(list(target.iterdir()),[]);self.assertEqual(target.stat().st_mode&0o777,0o700)

    def test_windows_destination_refuses_unrepresentable_linux_metadata(self):
        active=patch.object(jobs,'HELPER',self.f.helper);active.start();self.addCleanup(active.stop)
        for directory in [False,True]:
            for op in [0,1]:
                source=self.f.file()
                if directory:source.unlink();source.mkdir();(source/'child').write_bytes(b'retain child')
                os.setxattr(source,USER,b'Linux-only metadata');target=self.f.dst/source.name
                windows_path=subprocess.check_output(['wslpath','-w',str(self.f.dst)]).decode().strip()
                result=self.f.n.job(op,[source],windows_path,policy=2,windows=(0,))
                self.assertEqual(result['errors'],1);self.assertEqual(result['events'][0][1],errno.ENOTSUP);self.assertTrue(source.exists());self.assertEqual(os.getxattr(source,USER),b'Linux-only metadata');self.assertFalse(target.exists());self.assertEqual(result['exported'],b'')
                if directory:self.assertEqual((source/'child').read_bytes(),b'retain child')

    def test_cross_metadata_payload_is_bound_to_source_digest(self):
        source=self.f.file();os.setxattr(source,USER,b'original metadata');target=self.f.dst/source.name;target.write_bytes(b'old');seen=[]
        original=self.f.c.send
        def altered(process,typ,body=b''):
            if typ==cross.NODE_TYPE:
                body=bytearray(body);body[-1]^=1;body=bytes(body);seen.append(True)
            original(process,typ,body)
        with patch.object(self.f.c,'send',altered):result=self.f.transfer('cross',source,op=1)
        self.assertTrue(seen);self.assertEqual(result['errors'],1);self.assertEqual(result['events'][0][1],errno.EIO)
        self.assertEqual(os.getxattr(source,USER),b'original metadata');self.assertEqual(target.read_bytes(),b'old');self.assertEqual(list(self.f.dst.glob('.openpilot-stage-*')),[])

    def test_metadata_limit_is_an_explicit_error_not_a_lossy_move(self):
        # tmpfs can carry a valid 64KiB attribute which exceeds the bounded
        # transfer metadata record once its name/header are included.
        source=self.f.dst/'large-source';source.write_bytes(b'large metadata');os.setxattr(source,USER,b'x'*65536)
        for engine in ['native','cross']:
            dest=self.f.n.src;old=self.f.dst;self.f.dst=self.f.n.dst=self.f.c.dst=dest
            try:result=self.f.transfer(engine,source,op=1)
            finally:self.f.dst=self.f.n.dst=self.f.c.dst=old
            self.assertEqual(result['errors'],1);self.assertEqual(result['events'][0][1],errno.E2BIG);self.assertTrue(source.exists());self.assertEqual(len(os.getxattr(source,USER)),65536);self.assertFalse((dest/source.name).exists())

if __name__=='__main__':unittest.main(verbosity=2)
