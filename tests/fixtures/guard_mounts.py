"""Actual guard mount-scope acceptance, launched only in a private namespace."""
import errno
import json
import os
from pathlib import Path
import select
import struct
import subprocess
import sys
import tempfile

helper, parent_namespace, fallback = sys.argv[1:]
fallback = bool(int(fallback))
namespace = os.readlink('/proc/self/ns/mnt')
assert namespace != parent_namespace, 'mounts must run in a distinct namespace'
assert all(not field.startswith(('shared:', 'master:', 'propagate_from:'))
           for line in Path('/proc/self/mountinfo').read_text().splitlines()
           for field in line.split(' - ')[0].split()[6:]), 'propagation must be private before mounting'
report = dict(namespace=namespace, parent_namespace=parent_namespace,
              private_before_mounts=True, fallback=fallback, cases=[])
HEAD = struct.Struct('<III')
procs = []

def read(p, count):
    data = b''
    while len(data) < count:
        assert select.select([p.stdout], [], [], 5)[0], 'guard reply deadline'
        part = os.read(p.stdout.fileno(), count-len(data))
        assert part, 'unexpected helper EOF'; data += part
    return data

def guard(path, anchored=False, device=0):
    env = dict(os.environ, OPENPILOT_TEST_MOUNT_HELPER=helper)
    command = [sys.executable, str(Path(__file__).with_name('no_openat2.py')), '--guard'] if fallback else [helper, '--guard']
    p = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)
    procs.append(p)
    assert HEAD.unpack(read(p, 12)) == (1, 16, 0)
    _, _, uid, _ = struct.unpack('<IIII', read(p, 16)); assert uid == os.getuid()
    body = struct.pack('<QII', device, anchored, 0)+os.fsencode(path)
    p.stdin.write(HEAD.pack(43, len(body), 1)+body); p.stdin.flush()
    assert HEAD.unpack(read(p, 12)) == (44, 24, 1)
    dev, error, pid, fd, pad = struct.unpack('<QIIII', read(p, 24)); assert pad == 0
    if error: assert (dev, pid, fd) == (0, 0, 0), 'refusal must not publish a usable pin'
    report['cases'].append(dict(path=str(path), anchored=anchored, device=dev, error=error, pid=pid, fd=fd))
    return error, dev, f'/proc/{pid}/fd/{fd}'

def identity(path):
    value = path.stat()
    return path.read_bytes(), value.st_ino, value.st_mode, value.st_mtime_ns

with tempfile.TemporaryDirectory(prefix='openpilot_guard_mount_') as name:
    root = Path(name); top = root/'scope'; top.mkdir(); (top/'ordinary').mkdir()
    outside = root/'outside'; outside.mkdir(); (outside/'sentinel').write_bytes(b'retain outside bytes')
    (outside/'inside').mkdir(); (top/'link').symlink_to(outside, target_is_directory=True)
    unrelated = root/'unrelated'; unrelated.write_bytes(b'retain unrelated bytes')
    untouched = identity(unrelated); outside_before = identity(outside/'sentinel')
    mounts = []
    try:
        for kind in ['bind', 'tmpfs']:
            target = top/kind; target.mkdir()
            command = ['mount', '--bind', str(outside), str(target)] if kind == 'bind' else ['mount', '-t', 'tmpfs', '-o', 'size=1m', 'none', str(target)]
            subprocess.run(command, check=True, capture_output=True); mounts.append(target)
            if kind == 'tmpfs':
                (target/'sentinel').write_bytes(b'retain mounted bytes'); (target/'inside').mkdir()
            assert (target.stat().st_dev == top.stat().st_dev) == (kind == 'bind')
            before = identity(target/'sentinel')
            error, dev, anchor = guard(top); assert error == 0
            assert guard(anchor+'/ordinary', True, dev)[0] == 0
            assert guard(anchor+'/link', True, dev)[0] != 0
            assert guard(anchor+'/'+kind, True, dev)[0] == errno.EXDEV
            # Explicitly selected mounted endpoints are legal browsing roots.
            error, mounted_dev, mounted_anchor = guard(target); assert error == 0
            assert guard(mounted_anchor+'/inside', True, mounted_dev)[0] == 0
            assert identity(target/'sentinel') == before
            assert identity(outside/'sentinel') == outside_before
            assert identity(unrelated) == untouched
        report['sentinels_unchanged'] = True
    finally:
        # Release all pins before unmounting; mounts never leave this namespace.
        for p in reversed(procs):
            if not p.stdin.closed: p.stdin.close()
            try: p.wait(timeout=5)
            finally:
                if p.poll() is None: p.kill(); p.wait()
                p.stdout.close(); p.stderr.close()
        for target in reversed(mounts): subprocess.run(['umount', '--', str(target)], check=True, capture_output=True)
print(json.dumps(report))
