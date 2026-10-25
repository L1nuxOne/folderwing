"""VM-harness contracts and private real-writer stops; no VM is interrupted here."""
import copy
import contextlib
import importlib.util
import json
import os
from pathlib import Path
import shlex
import signal
import struct
import subprocess
import tarfile
import tempfile
import unittest
from unittest.mock import patch

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('vm_guest',ROOT/'tools/vm-recovery-guest.py')
guest=importlib.util.module_from_spec(spec);spec.loader.exec_module(guest)
spec=importlib.util.spec_from_file_location('vm_bundle',ROOT/'tools/vm-recovery-bundle.py')
bundler=importlib.util.module_from_spec(spec);spec.loader.exec_module(bundler)
LAB='12'*16;CASE='34'*16;BOOT='11111111-2222-3333-4444-555555555555'
IDENTITIES=[{'uuid':'11111111-1111-1111-1111-111111111111','device':1},
            {'uuid':'22222222-2222-2222-2222-222222222222','device':2}]

class RecoveryContracts(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory(prefix='openpilot_vm_contract_');self.addCleanup(self.temp.cleanup)
        self.root=Path(self.temp.name);self.src=self.root/'src';self.dst=self.root/'dst';self.src.mkdir();self.dst.mkdir()
        self.fixture=self.root/'fixture';self.fixture.write_bytes(b'private fixture')
        for root in (self.src,self.dst):(root/guest.MARKER).write_text('{}')
        self.owner_patch=patch.object(guest,'owned_volumes',return_value=IDENTITIES);self.owner_patch.start();self.addCleanup(self.owner_patch.stop)
        self.boot_patch=patch.object(guest,'boot_id',return_value=BOOT);self.boot_patch.start();self.addCleanup(self.boot_patch.stop)
        self.lock_patch=patch.object(guest,'guest_lock',return_value=contextlib.nullcontext());self.lock_patch.start();self.addCleanup(self.lock_patch.stop)

    def model(self,phase='partial-write'):
        return {'phase':phase,'new_sha256':guest.sha_bytes(guest.NEW),'old_sha256':guest.sha_bytes(guest.OLD)}

    def prepare(self,phase='partial-write'):
        return guest.prepare(self.src,self.dst,LAB,CASE,phase,self.fixture)

    def test_prepublication_requires_old_destination_and_retained_source(self):
        for phase in guest.PHASES[:3]:
            m=self.model(phase)
            self.assertEqual(guest.evaluate(m,m['new_sha256'],m['old_sha256'])['outcome'],'passed')
            self.assertEqual(guest.evaluate(m,m['new_sha256'],m['new_sha256'])['outcome'],'failed')
            self.assertEqual(guest.evaluate(m,None,m['old_sha256'])['outcome'],'failed')

    def test_unfenced_publication_allows_old_or_new_complete_destination(self):
        m=self.model('before-destination-sync')
        for target in (m['new_sha256'],m['old_sha256']):self.assertEqual(guest.evaluate(m,m['new_sha256'],target)['outcome'],'passed')
        for target in (None,'corrupt'):self.assertEqual(guest.evaluate(m,m['new_sha256'],target)['outcome'],'failed')

    def test_fenced_publication_requires_complete_destination_before_unlink(self):
        for phase in ('after-publication-sync','before-source-unlink'):
            m=self.model(phase)
            self.assertEqual(guest.evaluate(m,m['new_sha256'],m['new_sha256'])['outcome'],'passed')
            self.assertEqual(guest.evaluate(m,m['new_sha256'],m['old_sha256'])['outcome'],'failed')
            self.assertEqual(guest.evaluate(m,None,m['new_sha256'])['outcome'],'failed')

    def test_unfenced_source_unlink_allows_both_namespace_outcomes(self):
        m=self.model('after-source-unlink')
        for source in (None,m['new_sha256']):self.assertEqual(guest.evaluate(m,source,m['new_sha256'])['outcome'],'passed')
        self.assertEqual(guest.evaluate(m,'corrupt',m['new_sha256'])['outcome'],'failed')

    def test_fenced_source_unlink_requires_source_absence(self):
        m=self.model('after-source-sync')
        self.assertEqual(guest.evaluate(m,None,m['new_sha256'])['outcome'],'passed')
        self.assertEqual(guest.evaluate(m,m['new_sha256'],m['new_sha256'])['outcome'],'failed')
        self.assertEqual(guest.evaluate(m,None,None)['outcome'],'failed')

    def test_unknown_checkpoint_cannot_be_accepted(self):
        with self.assertRaises(ValueError):guest.evaluate(self.model('unknown'),None,None)
        with self.assertRaises(ValueError):self.prepare('unknown')

    def test_tokens_are_exact_canonical_uuids(self):
        self.assertEqual(guest.token(LAB),LAB)
        for value in ('../other','x'*32,LAB.upper().replace('1','A'),str(__import__('uuid').UUID(LAB))):
            with self.assertRaises(ValueError):guest.token(value)

    def test_duplicate_manifest_fields_are_rejected(self):
        with self.assertRaises(ValueError):json.loads('{"phase":"one","phase":"two"}',object_pairs_hook=guest.unique)

    def test_manifest_symlink_is_rejected(self):
        path=self.root/'original.json';path.write_text('{}');alias=self.root/'alias.json';alias.symlink_to(path)
        with self.assertRaises(ValueError):guest.read_json(alias)

    def test_baseline_creation_never_overwrites_an_existing_file(self):
        path=self.src/'existing';path.write_bytes(b'keep')
        with self.assertRaises(FileExistsError):guest.write_new(path,b'replacement')
        self.assertEqual(path.read_bytes(),b'keep')

    def test_prepare_binds_payload_volume_fixture_and_boot_identity(self):
        m=self.prepare();path=self.src/CASE/'case.json'
        self.assertEqual(guest.validate_manifest(path),m)
        self.assertEqual(m['filesystem_uuids'],[v['uuid'] for v in IDENTITIES])
        self.assertEqual(m['preparation_boot_id'],BOOT)
        self.assertEqual(guest.file_hash(Path(m['source'])),m['new_sha256'])
        self.assertEqual(guest.file_hash(Path(m['target'])),m['old_sha256'])

    def test_case_reuse_is_rejected_without_modifying_prior_data(self):
        m=self.prepare();before=Path(m['source']).read_bytes()
        with self.assertRaises(ValueError):self.prepare()
        self.assertEqual(Path(m['source']).read_bytes(),before)

    def test_manifest_path_substitution_is_rejected(self):
        m=self.prepare();m['target']=str(self.root/'foreign');path=self.src/CASE/'case.json';path.write_text(json.dumps(m))
        with self.assertRaises(ValueError):guest.validate_manifest(path)

    def test_changed_filesystem_identity_is_rejected(self):
        self.prepare()
        with patch.object(guest,'owned_volumes',return_value=[IDENTITIES[1],IDENTITIES[0]]):
            with self.assertRaises(ValueError):guest.validate_manifest(self.src/CASE/'case.json')

    def test_missing_and_reparse_recovered_files_are_distinguished(self):
        self.assertIsNone(guest.file_hash(self.root/'missing'))
        path=self.src/'link';path.symlink_to(self.fixture)
        with self.assertRaises(OSError):guest.file_hash(path)

    def test_unknown_or_oversized_recovered_objects_are_not_read(self):
        with self.assertRaises(ValueError):guest.file_hash(self.src)
        path=self.src/'large';path.write_bytes(b'x'*(len(guest.NEW)+1))
        with self.assertRaises(ValueError):guest.file_hash(path)

    def test_recovered_fifo_is_rejected_without_waiting_for_a_writer(self):
        path=self.src/'fifo';os.mkfifo(path)
        with self.assertRaises(ValueError):guest.file_hash(path)

    def test_wsl_is_rejected_before_guest_volume_mutation(self):
        original=Path.read_text
        def text(path,*args,**kwargs):
            return '6.18-microsoft-standard-WSL2' if str(path)=='/proc/sys/kernel/osrelease' else original(path,*args,**kwargs)
        with patch.object(Path,'read_text',text):
            with self.assertRaisesRegex(ValueError,'WSL'):guest.require_guest()

    def test_non_hyperv_guest_is_rejected(self):
        values={'/proc/sys/kernel/osrelease':'6.8.0','/sys/class/dmi/id/sys_vendor':'Other','/sys/class/dmi/id/product_name':'Virtual Machine'}
        with patch.object(Path,'read_text',lambda p,*a,**k:values[str(p)]):
            with self.assertRaises(ValueError):guest.require_guest()

    def test_guest_lock_cannot_be_used_to_bypass_wsl_guard(self):
        self.lock_patch.stop()
        original=Path.read_text
        with patch.object(Path,'read_text',lambda p,*a,**k:'6.18-WSL2' if str(p)=='/proc/sys/kernel/osrelease' else original(p,*a,**k)):
            with self.assertRaisesRegex(ValueError,'WSL'):
                with guest.guest_lock(LAB):self.fail('entered guest lab lock inside WSL')

    def test_guest_lab_lock_excludes_parallel_cases(self):
        self.lock_patch.stop()
        with patch.object(guest,'require_guest'),patch.object(guest,'RUN_ROOT',self.root):
            with guest.guest_lock(LAB):
                with self.assertRaises(BlockingIOError):
                    with guest.guest_lock(LAB):self.fail('parallel case entered lab')
            with guest.guest_lock(LAB):pass

    def test_same_boot_cannot_be_reported_as_vm_recovery(self):
        self.prepare()
        with patch.object(__import__('sys'),'argv',['guest','verify','--manifest',str(self.src/CASE/'case.json')]),patch.object(guest,'emit') as emit:
            self.assertEqual(guest.main(),2);emit.assert_not_called()

    def test_recovery_observation_preserves_staging_for_review(self):
        m=self.prepare();stage=Path(m['target']).parent/('.openpilot-stage-'+'a'*32);stage.mkdir();payload=stage/'payload';payload.write_bytes(b'partial')
        with patch.object(guest,'boot_id',return_value='66666666-7777-8888-9999-aaaaaaaaaaaa'),patch.object(__import__('sys'),'argv',['guest','verify','--manifest',str(self.src/CASE/'case.json')]),patch.object(guest,'emit') as emit:
            self.assertEqual(guest.main(),0)
            event=emit.call_args[0][0];self.assertTrue(event['observation_only']);self.assertEqual(event['staging_candidates'],[stage.name])
        self.assertEqual(payload.read_bytes(),b'partial');self.assertEqual(Path(m['source']).read_bytes(),guest.NEW)


class FixtureCheckpoints(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp=tempfile.TemporaryDirectory(prefix='openpilot_vm_fixture_build_');cls.addClassCleanup(cls.temp.cleanup)
        cls.fixture=Path(cls.temp.name)/'vm-fixture'
        subprocess.run([*shlex.split(os.environ.get('CC','cc')),'-std=c11','-O2','-pthread','-I'+str(ROOT/'src'),str(ROOT/'linux/fpjobs.c'),str(ROOT/'tests/fixtures/vm_recovery.c'),'-Wl,--wrap=fsync','-Wl,--wrap=write','-Wl,--wrap=unlinkat','-o',str(cls.fixture)],check=True,capture_output=True)

    def model(self,phase,source_root,destination_root):
        source=source_root/'source.bin';target=destination_root/'source.bin';source.write_bytes(guest.NEW);target.write_bytes(guest.OLD)
        self.assertNotEqual(source.stat().st_dev,target.stat().st_dev)
        return {'phase':phase,'source':str(source),'target':str(target),'new_sha256':guest.sha_bytes(guest.NEW),'old_sha256':guest.sha_bytes(guest.OLD),'fixture_sha256':guest.sha_bytes(self.fixture.read_bytes())}

    def stop(self,process):
        if process.poll() is None:process.kill()
        process.wait(timeout=5)
        for stream in (process.stdin,process.stdout,process.stderr):stream.close()

    def test_actual_writer_reaches_all_eight_stopped_checkpoints(self):
        for phase in guest.PHASES:
            with self.subTest(phase=phase),tempfile.TemporaryDirectory(prefix='openpilot_vm_source_') as src,tempfile.TemporaryDirectory(prefix='openpilot_vm_dest_',dir='/dev/shm') as dst:
                m=self.model(phase,Path(src),Path(dst));process,event=guest.run_to_checkpoint(m,self.fixture)
                try:
                    self.assertEqual(event['phase'],phase)
                    result=guest.evaluate(m,guest.file_hash(Path(m['source'])),guest.file_hash(Path(m['target'])))
                    self.assertEqual(result['outcome'],'passed')
                    if phase in guest.PHASES[4:]:self.assertEqual((event['destination_parent_synced'],event['stage_parent_synced']),(1,1))
                    if phase in guest.PHASES[6:]:self.assertEqual(event['source_unlinked'],1)
                finally:self.stop(process)

    def test_continued_fixture_exits_without_finishing_the_transfer(self):
        with tempfile.TemporaryDirectory() as src,tempfile.TemporaryDirectory(dir='/dev/shm') as dst:
            m=self.model('after-payload-sync',Path(src),Path(dst));process,event=guest.run_to_checkpoint(m,self.fixture)
            try:
                process.send_signal(signal.SIGCONT);self.assertEqual(process.wait(timeout=5),89)
                self.assertEqual(Path(m['source']).read_bytes(),guest.NEW);self.assertEqual(Path(m['target']).read_bytes(),guest.OLD)
            finally:self.stop(process)

    def test_changed_fixture_is_rejected_before_spawning_a_worker(self):
        with tempfile.TemporaryDirectory() as src,tempfile.TemporaryDirectory(dir='/dev/shm') as dst:
            m=self.model('partial-write',Path(src),Path(dst));m['fixture_sha256']='0'*64
            with self.assertRaisesRegex(ValueError,'fixture hash'):guest.run_to_checkpoint(m,self.fixture)
            self.assertEqual(Path(m['source']).read_bytes(),guest.NEW)

    def test_changed_baseline_is_rejected_before_transfer_mutation(self):
        with tempfile.TemporaryDirectory() as src,tempfile.TemporaryDirectory(dir='/dev/shm') as dst:
            m=self.model('partial-write',Path(src),Path(dst));Path(m['target']).write_bytes(b'changed')
            with self.assertRaisesRegex(ValueError,'baseline'):guest.run_to_checkpoint(m,self.fixture)
            self.assertEqual(Path(m['target']).read_bytes(),b'changed')


class RecoveryBundle(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory(prefix='openpilot_vm_bundle_');self.addCleanup(self.temp.cleanup);self.root=Path(self.temp.name)

    def elf(self,interpreter=False):
        ident=b'\x7fELF\x02\x01\x01'+b'\0'*9
        data=struct.pack('<16sHHIQQQIHHHHHH',ident,2,62,1,0x400078,64,0,0,64,56,2 if interpreter else 1,0,0,0)
        data+=struct.pack('<IIQQQQQQ',1,5,0,0x400000,0,256,256,4096)
        if interpreter:data+=struct.pack('<IIQQQQQQ',3,4,200,0,0,8,8,1)
        return data.ljust(256,b'\0')

    def test_static_x86_64_fixture_header_is_checked(self):
        bundler.static_fixture(self.elf())
        with self.assertRaises(ValueError):bundler.static_fixture(b'not ELF')
        invalid=bytearray(self.elf());invalid[18:20]=b'\x03\x00'
        with self.assertRaises(ValueError):bundler.static_fixture(invalid)

    def test_dynamic_interpreter_is_rejected(self):
        with self.assertRaisesRegex(ValueError,'dynamic interpreter'):bundler.static_fixture(self.elf(True))

    def test_bundle_is_deterministic_and_content_bound(self):
        fixture=self.root/'fixture';fixture.write_bytes(self.elf());a=self.root/'a.tar.gz';b=self.root/'b.tar.gz'
        m=bundler.bundle(fixture,a);self.assertEqual(m,bundler.bundle(fixture,b));self.assertEqual(a.read_bytes(),b.read_bytes())
        with tarfile.open(a) as tar:
            for name,expected in m['files'].items():
                self.assertEqual(guest.sha_bytes(tar.extractfile('openpilot-vm-recovery/'+name).read()),expected)
            self.assertEqual(tar.getmember('openpilot-vm-recovery/fixture').mode,0o755)
        self.assertEqual(m['payload']['new_sha256'],guest.sha_bytes(guest.NEW));self.assertEqual(m['phases'],list(guest.PHASES))

    def test_bundle_never_overwrites_existing_evidence(self):
        fixture=self.root/'fixture';fixture.write_bytes(self.elf());out=self.root/'evidence';out.write_bytes(b'preserve')
        with self.assertRaises(FileExistsError):bundler.bundle(fixture,out)
        self.assertEqual(out.read_bytes(),b'preserve')

    def test_fixture_symlink_is_rejected(self):
        real=self.root/'real';real.write_bytes(self.elf());alias=self.root/'alias';alias.symlink_to(real)
        with self.assertRaises(ValueError):bundler.bundle(alias,self.root/'out.tar.gz')

if __name__=='__main__':unittest.main()
