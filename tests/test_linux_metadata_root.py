"""Engine integration: root-only Linux metadata and actual ACL access oracles.
Owners: Linux copy/import metadata contract; only owned synthetic data is used.
Run this gate explicitly as root; inability to exercise it is not a skip.
"""
import os
from pathlib import Path
import struct
import subprocess
import sys
import unittest
import test_linux_metadata as meta

HELPER=sys.argv.pop(1) if __name__=='__main__' and len(sys.argv)>1 and not sys.argv[1].startswith('-') else meta.HELPER

class RootMetadata(unittest.TestCase):
    def setUp(self):
        self.assertEqual(os.geteuid(),0,'Run the private metadata access gate as root')
        self.f=meta.MetadataFixtures(HELPER);self.addCleanup(self.f.close)
        for directory in [self.f.n.root,self.f.src,self.f.dst]:directory.chmod(0o755)
    def can_read_as_nobody(self,path):
        code='import os,sys;os.setgroups([]);os.setgid(65534);os.setuid(65534)\ntry:\n f=open(sys.argv[1],"rb");f.read();f.close()\nexcept PermissionError:sys.exit(7)\n'
        result=subprocess.run([sys.executable,'-c',code,str(path)],capture_output=True)
        self.assertIn(result.returncode,[0,7],result.stderr.decode());return result.returncode==0
    def test_inherited_default_acl_cannot_grant_new_read_access(self):
        os.setxattr(self.f.dst,meta.DEFAULT,meta.acl(owner=7,named=7,mask=7))
        for engine in ['native','cross']:
            for op in [0,1]:
                with self.subTest(engine=engine,op=op):
                    source=self.f.file();source.chmod(0o640);self.assertFalse(self.can_read_as_nobody(source))
                    result=self.f.transfer(engine,source,op=op);target=self.f.dst/source.name
                    self.assertEqual(result['errors'],0);self.assertFalse(self.can_read_as_nobody(target));self.assertEqual(meta.attributes(target),{});self.assertEqual(target.stat().st_mode&0o777,0o640)
    def test_named_acl_numeric_subject_access_is_preserved(self):
        for engine in ['native','cross']:
            source=self.f.file();os.setxattr(source,meta.ACCESS,meta.acl());self.assertTrue(self.can_read_as_nobody(source))
            result=self.f.transfer(engine,source,op=1);target=self.f.dst/source.name
            self.assertEqual(result['errors'],0);self.assertTrue(self.can_read_as_nobody(target));self.assertEqual(os.getxattr(target,meta.ACCESS),meta.acl());self.assertFalse(source.exists())
    def test_security_and_trusted_attributes_refuse_lossy_copies_and_moves(self):
        import errno
        for name,value in [('trusted.openpilot.audit',b'opaque metadata'),('security.capability',struct.pack('<IIIII',0x02000000,0,0,0,0))]:
            for engine in ['native','cross']:
                for op in [0,1]:
                    with self.subTest(name=name,engine=engine,op=op):
                        source=self.f.file();os.setxattr(source,name,value);target=self.f.dst/source.name;target.write_bytes(b'old')
                        result=self.f.transfer(engine,source,op=op)
                        self.assertEqual(result['errors'],1);self.assertEqual(result['events'][0][1],errno.ENOTSUP);self.assertEqual(os.getxattr(source,name),value);self.assertEqual(target.read_bytes(),b'old')
    def test_same_filesystem_rename_retains_unsupported_attributes(self):
        source=self.f.file();os.setxattr(source,'trusted.openpilot.audit',b'retained inode')
        dest=self.f.src/'same-volume';dest.mkdir();before=source.stat().st_ino;old=self.f.dst;self.f.dst=self.f.n.dst=dest
        try:result=self.f.transfer('native',source,op=1)
        finally:self.f.dst=self.f.n.dst=old
        target=dest/source.name;self.assertEqual(result['errors'],0);self.assertEqual(target.stat().st_ino,before);self.assertEqual(os.getxattr(target,'trusted.openpilot.audit'),b'retained inode')
    def test_merge_with_identical_directory_metadata_keeps_all_attributes(self):
        for engine in ['native','cross']:
            source=self.f.file();source.unlink();source.mkdir();(source/'child').write_bytes(b'complete child');target=self.f.dst/source.name;target.mkdir()
            for node in [source,target]:
                os.setxattr(node,meta.USER,b'matching directory');os.setxattr(node,meta.ACCESS,meta.acl(owner=7,named=5,mask=5));os.setxattr(node,meta.DEFAULT,meta.acl(owner=7,named=4,mask=4))
            before=meta.attributes(source);result=self.f.transfer(engine,source,op=1)
            self.assertEqual(result['errors'],0);self.assertFalse(source.exists());self.assertEqual(meta.attributes(target),before);self.assertEqual((target/'child').read_bytes(),b'complete child')

if __name__=='__main__':unittest.main(verbosity=2)
