#!/usr/bin/env python3
"""Read-only helper entry in a private user/mount namespace; never installed."""
import json, os, re, subprocess, sys
from pathlib import Path

root = Path(os.environ['OPENPILOT_MOUNT_ROOT'])
assert re.fullmatch(r'/tmp/openpilot-duplicate-mount-[0-9a-f]{32}', str(root))
assert not root.is_symlink() and root.stat().st_uid == os.getuid()
assert (root / '.owner').read_text() == 'openpilot-duplicate-mount/v1'
args = sys.argv[1:]
if args[:1] != ['--inside']:
    assert args in [['--analyze-space'], ['--duplicate-read']]
    assert not os.path.ismount(root / 'parent/mounted')
    os.environ['OPENPILOT_OUTER_MOUNT_NS'] = os.readlink('/proc/self/ns/mnt')
    os.execvp('unshare', ['unshare', '--user', '--map-root-user', '--mount',
                          '--propagation', 'private', sys.executable,
                          __file__, '--inside', *args])
args = args[1:]
assert args in [['--analyze-space'], ['--duplicate-read']]
outer = os.environ['OPENPILOT_OUTER_MOUNT_NS']
inner = os.readlink('/proc/self/ns/mnt')
assert inner != outer
source, target, parent = root/'source', root/'parent/mounted', root/'parent'
assert all(p.is_dir() and not p.is_symlink() for p in [source, target, parent])
assert not list(target.iterdir())
subprocess.run(['mount', '--bind', str(source), str(target)], check=True,
               stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL)
def mount_id(path):
    fd = os.open(path, os.O_PATH | os.O_NOFOLLOW)
    try:
        return next(line.split(':', 1)[1].strip() for line in
                    Path(f'/proc/self/fdinfo/{fd}').read_text().splitlines()
                    if line.startswith('mnt_id:'))
    finally:
        os.close(fd)
assert source.stat().st_ino == target.stat().st_ino
assert source.stat().st_dev == target.stat().st_dev == parent.stat().st_dev
assert mount_id(parent) != mount_id(target)
trace = dict(outer=outer, inner=inner, parent_mount=mount_id(parent),
             target_mount=mount_id(target), same_device=True, args=args)
(root/'traces'/f'{os.getpid()}.json').write_text(json.dumps(trace))
os.execv('./folderwing-wsl-real', ['./folderwing-wsl-real', *args])
