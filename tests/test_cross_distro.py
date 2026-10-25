"""Two native transfer helpers; private trees only. Windows UI covers real distros."""
import errno
import hashlib
import os
from pathlib import Path
import select
import struct
import subprocess
import sys
import tempfile
import unittest

HELPER = sys.argv.pop(1) if __name__ == '__main__' and len(sys.argv) > 1 and not sys.argv[1].startswith('-') else str(Path(__file__).resolve().parents[1] / 'build/release/folderwing-wsl')
HEAD = struct.Struct('<III')
PROGRESS = struct.Struct('<QQIIIIIiII')
NODE = struct.Struct('<QQQQqqIIIIII')
END = struct.Struct('<iIQ32s')
ACK = struct.Struct('<iIII')
TRANSFER, NODE_TYPE, DATA, FILE_END, ACK_TYPE, DIR_END, QUIT = range(27, 34)

class CrossDistro(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='openpilot_cross_')
        self.root = Path(self.tmp.name)
        self.src = self.root/'source'; self.src.mkdir()
        self.dst = self.root/'target'; self.dst.mkdir()
        self.children = []

    def tearDown(self):
        for p in self.children:
            if p.poll() is None: p.kill(); p.wait()
            for stream in [p.stdin, p.stdout, p.stderr]: stream.close()
        # Restore private zero-mode fixtures before recursive cleanup.
        for tree in [self.src, self.dst]:
            for folder, dirs, files in os.walk(tree):
                os.chmod(folder, 0o700)
                for name in dirs:
                    path = Path(folder)/name
                    if not path.is_symlink(): os.chmod(path, 0o700)
        self.tmp.cleanup()

    def send(self, p, typ, data=b''):
        p.stdin.write(HEAD.pack(typ, len(data), 1) + data); p.stdin.flush()

    def receive(self, p):
        def read(n):
            data = b''
            while len(data) < n:
                if not select.select([p.stdout], [], [], 10)[0]: self.fail('transfer helper reply timeout')
                part = os.read(p.stdout.fileno(), n-len(data))
                if not part: self.fail('transfer helper closed its pipe unexpectedly')
                data += part
            return data
        typ, n, ident = HEAD.unpack(read(12)); self.assertLessEqual(n, 1 << 20)
        return typ, ident, read(n)

    def start(self, importing, path, op=0, policy=0):
        p = subprocess.Popen([HELPER, '--import' if importing else '--export'], stdin=subprocess.PIPE,
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.children.append(p)
        typ, _, hello = self.receive(p); self.assertEqual(typ, 1); self.assertEqual(len(hello), 16)
        raw = os.fsencode(path); self.send(p, TRANSFER, struct.pack('<IIII', op, policy, len(raw), 0) + raw)
        return p

    def transfer(self, source, op=0, policy=0, mutate=None, corrupt_digest=False, ack_error=0, cancel_at=None, destination=None, directory_ack_missing=False, final_file_ack=None):
        receiver = self.start(True, destination or self.dst, op, policy)
        typ, _, body = self.receive(receiver); self.assertEqual(typ, ACK_TYPE)
        self.assertEqual(ACK.unpack(body)[0], 0)
        sender = self.start(False, source, op, policy)
        events = []; footer = []; cancelled = False; continued = False; streamed = 0
        while True:
            typ, ident, body = self.receive(sender); self.assertEqual(ident, 1)
            if typ == 11:
                values = PROGRESS.unpack(body[:48]); self.assertEqual(values[-1], len(body)-48)
                if values[2] == 2 and not continued:
                    continued = True
                    self.send(sender, 13 if cancel_at == 'running' else 14); cancelled = cancel_at == 'running'
                if values[2] >= 3:
                    sender.wait(timeout=5); self.assertEqual(sender.returncode, 0)
                    self.send(receiver, QUIT); receiver.wait(timeout=5); self.assertEqual(receiver.returncode, 0)
                    return dict(state=values[2], errors=values[5], skipped=values[6], events=events, footer=footer, streamed=streamed, items=values[3], total=values[4])
                continue
            if typ == 12:
                events.append(struct.unpack('<IiII', body[:16]) + (body[16:],)); continue
            self.assertIn(typ, [NODE_TYPE, DATA, FILE_END, DIR_END])
            if mutate: mutate(typ, body)
            if typ == DATA:
                self.assertLessEqual(len(body), 65536); streamed += len(body)
                if cancel_at == 'data' and not cancelled: self.send(sender, 13); cancelled = True
            if typ == FILE_END:
                footer.append(END.unpack(body))
                if corrupt_digest:
                    err, pad, count, digest = END.unpack(body); body = END.pack(err, pad, count, bytes([digest[0]^1]) + digest[1:])
            self.send(receiver, typ, body)
            if typ != DATA:
                ack_typ, _, ack = self.receive(receiver); self.assertEqual(ack_typ, ACK_TYPE)
                if typ == FILE_END and ack_error: ack = ACK.pack(ack_error, 0, 0, 0)
                if typ == FILE_END and final_file_ack is not None: ack = ACK.pack(*final_file_ack)
                if typ == DIR_END and directory_ack_missing: ack = ACK.pack(0, 0, 0, 0)
                self.send(sender, ACK_TYPE, ack)

    def test_regular_files_hash_boundaries_and_readback(self):
        for size in [0, 1, 55, 56, 63, 64, 65, 1025, 65536, 65537]:
            with self.subTest(size=size):
                p = self.src/f'bytes-{size}'; data = bytes((i*37)%256 for i in range(size)); p.write_bytes(data)
                r = self.transfer(p)
                self.assertEqual(r['errors'], 0); self.assertEqual((self.dst/p.name).read_bytes(), data)
                self.assertEqual(r['footer'][0][2:], (size, hashlib.sha256(data).digest()))
                self.assertTrue(p.exists()); self.assertEqual(r['streamed'], size)

    def test_metadata_and_literal_symlinks(self):
        tree = self.src/'tree'; tree.mkdir(); os.chmod(tree, 0o750)
        file = tree/'script'; file.write_bytes(b'run'); os.chmod(file, 0o754)
        ns = (1700000000123456789, 1700000000987654321); os.utime(file, ns=ns); os.utime(tree, ns=ns)
        (tree/'broken').symlink_to('missing'); (tree/'outside').symlink_to('/never/follow')
        r = self.transfer(tree); self.assertEqual(r['errors'], 0)
        copied = self.dst/'tree/script'; self.assertEqual(copied.stat().st_mode & 0o7777, 0o754)
        self.assertEqual(copied.stat().st_mtime_ns, ns[1]); self.assertEqual(copied.stat().st_atime_ns, ns[0])
        self.assertEqual((self.dst/'tree').stat().st_mode & 0o7777, 0o750)
        self.assertEqual(os.readlink(self.dst/'tree/broken'), 'missing'); self.assertEqual(os.readlink(self.dst/'tree/outside'), '/never/follow')

    def test_keep_both_skip_replace_and_case(self):
        p = self.src/'Foo.txt'; p.write_bytes(b'new'); target = self.dst/p.name; target.write_bytes(b'old')
        r = self.transfer(p, policy=1); self.assertEqual(r['skipped'], 1); self.assertEqual(target.read_bytes(), b'old')
        self.assertEqual(self.transfer(p)['errors'], 0); self.assertEqual((self.dst/'Foo (2).txt').read_bytes(), b'new')
        self.assertEqual(self.transfer(p, policy=2)['errors'], 0); self.assertEqual(target.read_bytes(), b'new')
        q = self.src/'foo.txt'; q.write_bytes(b'lower'); self.assertEqual(self.transfer(q)['errors'], 0)
        self.assertEqual((self.dst/q.name).read_bytes(), b'lower')

    def test_move_commits_before_guarded_source_removal(self):
        p = self.src/'move'; p.write_bytes(b'payload'); r = self.transfer(p, op=1)
        self.assertEqual(r['errors'], 0); self.assertFalse(p.exists()); self.assertEqual((self.dst/p.name).read_bytes(), b'payload')

    def test_corrupt_digest_aborts_replace_and_retains_move_source(self):
        p = self.src/'file'; p.write_bytes(b'new'); old = self.dst/p.name; old.write_bytes(b'old')
        r = self.transfer(p, op=1, policy=2, corrupt_digest=True)
        self.assertEqual(r['errors'], 1); self.assertTrue(p.exists()); self.assertEqual(old.read_bytes(), b'old')
        self.assertEqual(list(self.dst.iterdir()), [old])

    def test_negative_destination_ack_retains_source(self):
        p = self.src/'file'; p.write_bytes(b'data'); r = self.transfer(p, op=1, ack_error=errno.EIO)
        self.assertEqual(r['errors'], 1); self.assertTrue(p.exists()); self.assertEqual((self.dst/p.name).read_bytes(), b'data')

    # Linux integration / final-ACK diagnostics: linux/fptransfer.c export_node,
    # wslproto.h commit/error fields and this module's protocol relay fixture.
    def test_regular_file_requires_a_committed_final_ack(self):
        for op in [0, 1]:
            for data in [b'', b'verified but not acknowledged']:
                with self.subTest(op=op, size=len(data)):
                    p = self.src/'file'; p.write_bytes(data)
                    target = self.dst/p.name; target.write_bytes(b'old destination')
                    r = self.transfer(p, op=op, policy=2, final_file_ack=(0, 0, 0, 0))
                    self.assertEqual((r['state'], r['errors'], r['skipped']), (3, 1, 0))
                    self.assertEqual((r['items'], r['total']), (1, 1))
                    self.assertEqual(len(r['events']), 1)
                    self.assertEqual(r['events'][0][:3], (0, errno.EIO, 1))
                    self.assertEqual(p.read_bytes(), data)
                    self.assertEqual(target.read_bytes(), data)

    def test_final_file_ack_preserves_error_skip_and_cancel(self):
        cases = [
            ((errno.ENOSPC, 0, 0, 0), (3, 1, 0), (0, errno.ENOSPC)),
            ((0, 1, 0, 0), (3, 0, 1), (1, errno.EEXIST)),
            ((errno.ECANCELED, 0, 0, 0), (4, 0, 0), None),
        ]
        for ack, counts, diagnostic in cases:
            with self.subTest(ack=ack):
                p = self.src/'file'; p.write_bytes(b'retain source')
                r = self.transfer(p, op=1, policy=2, final_file_ack=ack)
                self.assertEqual((r['state'], r['errors'], r['skipped']), counts)
                self.assertEqual(len(r['events']), int(diagnostic is not None))
                if diagnostic: self.assertEqual(r['events'][0][:2], diagnostic)
                self.assertEqual(p.read_bytes(), b'retain source')
                self.assertEqual((self.dst/p.name).read_bytes(), b'retain source')

    def test_source_error_survives_a_final_ack_claiming_no_error(self):
        for committed in [0, 1]:
            with self.subTest(committed=committed):
                p = self.src/'file'; p.write_bytes(b'a'*131072)
                target = self.dst/p.name; target.write_bytes(b'old destination')
                changed = False
                def mutate(typ, body):
                    nonlocal changed
                    if typ == DATA and not changed:
                        p.write_bytes(b'changed during stream'); changed = True
                r = self.transfer(p, op=1, policy=2, mutate=mutate, final_file_ack=(0, 0, committed, 0))
                self.assertTrue(changed); self.assertEqual(r['footer'][0][0], errno.ESTALE)
                self.assertEqual((r['errors'], r['skipped']), (1, 0))
                self.assertEqual(len(r['events']), 1)
                self.assertEqual(r['events'][0][:2], (0, errno.ESTALE))
                self.assertEqual(p.read_bytes(), b'changed during stream')
                self.assertEqual(target.read_bytes(), b'old destination')

    def test_source_changed_during_read_is_not_committed_or_deleted(self):
        p = self.src/'file'; p.write_bytes(b'a'*131072); old = self.dst/p.name; old.write_bytes(b'old'); changed = False
        def mutate(typ, body):
            nonlocal changed
            if typ == DATA and not changed:
                p.write_bytes(b'replacement'); changed = True
        r = self.transfer(p, op=1, policy=2, mutate=mutate)
        self.assertEqual(r['errors'], 1); self.assertEqual(p.read_bytes(), b'replacement'); self.assertEqual(old.read_bytes(), b'old')

    def test_destination_changed_before_commit_is_preserved(self):
        p = self.src/'file'; p.write_bytes(b'new'); target = self.dst/p.name; target.write_bytes(b'old')
        def mutate(typ, body):
            if typ == FILE_END: target.write_bytes(b'concurrent change')
        r = self.transfer(p, op=1, policy=2, mutate=mutate)
        self.assertEqual(r['errors'], 1); self.assertTrue(p.exists()); self.assertEqual(target.read_bytes(), b'concurrent change')

    def test_directory_merge_move_retains_skipped_children(self):
        tree = self.src/'tree'; tree.mkdir(); (tree/'same').write_text('new'); (tree/'extra').write_text('extra')
        dst = self.dst/'tree'; dst.mkdir(); (dst/'same').write_text('old')
        r = self.transfer(tree, op=1, policy=1)
        self.assertEqual(r['errors'], 0); self.assertEqual(r['skipped'], 1)
        self.assertTrue((tree/'same').exists()); self.assertFalse((tree/'extra').exists())
        self.assertEqual((dst/'same').read_text(), 'old'); self.assertEqual((dst/'extra').read_text(), 'extra')

    def test_partial_move_refuses_special_nodes_without_traversal(self):
        tree = self.src/'tree'; tree.mkdir(); (tree/'good').write_text('good'); os.mkfifo(tree/'pipe')
        r = self.transfer(tree, op=1)
        self.assertEqual(r['errors'], 1); self.assertTrue((tree/'pipe').exists()); self.assertFalse((tree/'good').exists())
        self.assertEqual((self.dst/'tree/good').read_text(), 'good'); self.assertFalse((self.dst/'tree/pipe').exists())

    def test_raw_linux_names_roundtrip(self):
        raw = os.fsencode(self.src) + b'/colon:quote\'\ninvalid\xff'
        with open(raw, 'wb') as f: f.write(b'literal')
        r = self.transfer(raw); self.assertEqual(r['errors'], 0)
        self.assertEqual(os.listdir(os.fsencode(self.dst)), [raw.rsplit(b'/', 1)[1]])

    def test_cancel_before_mutation_and_during_file_removes_temp(self):
        p = self.src/'large'; p.write_bytes(b'x'*131072); target = self.dst/p.name; target.write_bytes(b'old')
        for point in ['running', 'data']:
            r = self.transfer(p, op=1, policy=2, cancel_at=point)
            self.assertEqual(r['state'], 4); self.assertTrue(p.exists()); self.assertEqual(target.read_bytes(), b'old')
            self.assertEqual(list(self.dst.iterdir()), [target])

    def test_destination_symlink_and_source_alias_are_refused(self):
        p = self.src/'file'; p.write_text('data'); keep = self.root/'keep'; keep.write_text('safe'); (self.dst/'file').symlink_to(keep)
        self.assertEqual(self.transfer(p, op=1, policy=2)['errors'], 1); self.assertTrue(p.exists()); self.assertEqual(keep.read_text(), 'safe')
        nested = self.src/'nested'; nested.mkdir()
        self.assertEqual(self.transfer(self.src, destination=nested)['errors'], 1); self.assertEqual(list(nested.iterdir()), [])

    def test_destination_denial_retains_source_without_elevation(self):
        self.assertNotEqual(os.geteuid(), 0, 'This gate uses the unprivileged default WSL user')
        p = self.src/'denied'; p.write_text('data'); os.chmod(self.dst, 0o500)
        try:
            r = self.transfer(p, op=1)
            self.assertEqual(r['errors'], 1); self.assertTrue(p.exists()); self.assertEqual(list(self.dst.iterdir()), [])
            self.assertEqual(r['events'][0][1], errno.EACCES)
            self.assertEqual((r['items'], r['total']), (1, 1))
        finally: os.chmod(self.dst, 0o700)

    def test_source_changed_after_verified_ack_is_not_removed(self):
        p = self.src/'guarded'; p.write_bytes(b'original')
        receiver = self.start(True, self.dst, 1); self.assertEqual(ACK.unpack(self.receive(receiver)[2])[0], 0)
        sender = self.start(False, p, 1); continued = False
        while True:
            typ, _, body = self.receive(sender)
            if typ == 11:
                state = PROGRESS.unpack(body[:48])[2]
                if state == 2 and not continued: self.send(sender, 14); continued = True
                if state >= 3:
                    values = PROGRESS.unpack(body[:48]); self.assertEqual(values[5], 1)
                    break
            elif typ != 12:
                self.send(receiver, typ, body)
                if typ != DATA:
                    _, _, ack = self.receive(receiver)
                    if typ == FILE_END: p.write_bytes(b'changed after verification')
                    self.send(sender, ACK_TYPE, ack)
        self.send(receiver, QUIT); sender.wait(timeout=5); receiver.wait(timeout=5)
        self.assertEqual(p.read_bytes(), b'changed after verification'); self.assertEqual((self.dst/p.name).read_bytes(), b'original')

    def test_invalid_components_and_malformed_nodes_never_escape_root(self):
        p = self.start(True, self.dst); self.assertEqual(ACK.unpack(self.receive(p)[2])[0], 0)
        for name in [b'../escape', b'.', b'..', b'', b'a\0b', b'a/b']:
            node = NODE.pack(0, 0, 0, 0, 0, 0, 0o100644, os.getuid(), os.getgid(), len(name), 0, 0) + name
            self.send(p, NODE_TYPE, node); typ, _, body = self.receive(p)
            self.assertEqual(typ, ACK_TYPE); self.assertEqual(ACK.unpack(body)[0], errno.EINVAL)
        self.send(p, QUIT); p.wait(timeout=5); self.assertEqual(list(self.dst.iterdir()), [])

    def test_directory_move_requires_a_committed_final_ack(self):
        tree = self.src/'tree'; tree.mkdir(); (tree/'file').write_bytes(b'verified')
        r = self.transfer(tree, op=1, directory_ack_missing=True)
        self.assertEqual(r['errors'], 1); self.assertTrue(tree.is_dir())
        self.assertEqual(list(tree.iterdir()), []); self.assertEqual((self.dst/'tree/file').read_bytes(), b'verified')

    def test_import_substituted_payload_never_commits_or_removes_source(self):
        for replacement in [b'authentic source', b'unverified replacement']:
            with self.subTest(replacement=replacement):
                source = self.src/'selected'; source.write_bytes(b'authentic source')
                target = self.dst/source.name; target.write_bytes(b'old destination')
                foreign = []
                def substitute(typ, body):
                    if typ != FILE_END: return
                    stages = list(self.dst.glob('.openpilot-stage-*')); self.assertEqual(len(stages), 1)
                    payload = stages[0]/'payload'; identity = payload.stat().st_ino
                    payload.unlink(); payload.write_bytes(replacement)
                    self.assertNotEqual(payload.stat().st_ino, identity); foreign.append(payload)
                result = self.transfer(source, op=1, policy=2, mutate=substitute)
                self.assertEqual(result['errors'], 1)
                self.assertEqual(source.read_bytes(), b'authentic source')
                self.assertEqual(target.read_bytes(), b'old destination')
                self.assertEqual(foreign[0].read_bytes(), replacement)
                # This retained foreign entry belongs to our fixture, not helper cleanup.
                foreign[0].unlink(); foreign[0].parent.rmdir()

    def test_import_substituted_staging_directory_is_not_cleaned_as_ours(self):
        source = self.src/'selected'; source.write_bytes(b'authentic source')
        target = self.dst/source.name; target.write_bytes(b'old destination')
        replacements = []
        def substitute(typ, body):
            if typ != FILE_END: return
            stages = list(self.dst.glob('.openpilot-stage-*')); self.assertEqual(len(stages), 1)
            stage = stages[0]; kept = self.dst/'renamed-stage'; stage.rename(kept)
            stage.mkdir(mode=0o700); marker = stage/'foreign'; marker.write_bytes(b'keep me')
            replacements.append((kept, marker))
        result = self.transfer(source, op=1, policy=2, mutate=substitute)
        self.assertEqual(result['errors'], 1)
        self.assertEqual(source.read_bytes(), b'authentic source')
        self.assertEqual(target.read_bytes(), b'old destination')
        kept, marker = replacements[0]
        self.assertEqual(marker.read_bytes(), b'keep me'); self.assertEqual(list(kept.iterdir()), [])

    def test_import_staging_permissions_and_eof_preserve_substituted_payload(self):
        target = self.dst/'file'; target.write_bytes(b'old destination')
        p = self.start(True, self.dst, op=1, policy=2)
        self.assertEqual(ACK.unpack(self.receive(p)[2])[0], 0)
        name = b'file'
        node = NODE.pack(100, 0, 0, 0, 0, 0, 0o100644, os.getuid(), os.getgid(), len(name), 0, 0) + name
        self.send(p, NODE_TYPE, node); self.assertEqual(ACK.unpack(self.receive(p)[2])[0], 0)
        stages = list(self.dst.glob('.openpilot-stage-*')); self.assertEqual(len(stages), 1)
        stage = stages[0]; payload = stage/'payload'
        self.assertEqual(stage.stat().st_mode & 0o777, 0o700)
        self.assertEqual(stage.stat().st_uid, os.geteuid())
        self.assertEqual(payload.stat().st_mode & 0o777, 0o600)
        self.send(p, DATA, b'unfinished')
        identity = payload.stat().st_ino; payload.unlink(); payload.write_bytes(b'foreign object')
        self.assertNotEqual(payload.stat().st_ino, identity)
        p.stdin.close(); p.wait(timeout=5)
        self.assertEqual(payload.read_bytes(), b'foreign object')
        self.assertEqual(target.read_bytes(), b'old destination')

    def test_import_reuses_private_workspace_and_refuses_injected_payload(self):
        for foreign in [False, True]:
            with self.subTest(foreign=foreign):
                root = self.dst/str(foreign); root.mkdir()
                p = self.start(True, root); self.assertEqual(ACK.unpack(self.receive(p)[2])[0], 0)
                def node(name, data):
                    raw = name.encode()
                    body = NODE.pack(len(data), 0, 0, 0, 0, 0, 0o100644, os.getuid(), os.getgid(), len(raw), 0, 0) + raw
                    self.send(p, NODE_TYPE, body)
                    return ACK.unpack(self.receive(p)[2])
                data = b'verified first file'
                self.assertEqual(node('first', data)[0], 0)
                stage, = root.glob('.openpilot-stage-*'); identity = stage.stat().st_ino
                self.send(p, DATA, data)
                self.send(p, FILE_END, END.pack(0, 0, len(data), hashlib.sha256(data).digest()))
                ack = ACK.unpack(self.receive(p)[2]); self.assertEqual((ack[0], ack[2]), (0, 1))
                self.assertEqual(list(stage.iterdir()), [])
                if foreign: (stage/'payload').write_bytes(b'foreign between files')
                ack = node('second', b'unfinished second')
                self.assertEqual(ack[0], errno.EEXIST if foreign else 0)
                self.assertEqual(stage.stat().st_ino, identity)
                self.assertEqual(list(root.glob('.openpilot-stage-*')), [stage])
                p.stdin.close(); p.wait(timeout=5)
                self.assertEqual((root/'first').read_bytes(), data); self.assertFalse((root/'second').exists())
                if foreign: self.assertEqual((stage/'payload').read_bytes(), b'foreign between files')
                else: self.assertFalse(stage.exists())

    def test_import_eof_cleans_unfinished_temporary(self):
        p = self.start(True, self.dst); self.assertEqual(ACK.unpack(self.receive(p)[2])[0], 0)
        name = b'file'; node = NODE.pack(100, 0, 0, 0, 0, 0, 0o100644, os.getuid(), os.getgid(), len(name), 0, 0) + name
        self.send(p, NODE_TYPE, node); self.assertEqual(ACK.unpack(self.receive(p)[2])[0], 0)
        self.send(p, DATA, b'unfinished'); p.stdin.close(); p.wait(timeout=5); self.assertEqual(list(self.dst.iterdir()), [])

if __name__ == '__main__': unittest.main()
