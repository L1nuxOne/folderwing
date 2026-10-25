"""Marker-guarded fixture setup/check/cleanup; mounts never occur in this process."""
import json, os, re, shlex, shutil, sys
from pathlib import Path
action, raw = sys.argv[1:3]
assert re.fullmatch(r'/tmp/openpilot-duplicate-mount-[0-9a-f]{32}', raw)
root = Path(raw)
if action == 'setup':
    root.mkdir(mode=0o700)
    (root/'.owner').write_text('openpilot-duplicate-mount/v1')
    for name in ['parent', 'parent/mounted', 'parent/ordinary', 'source', 'source/nested', 'traces']:
        (root/name).mkdir()
    (root/'parent/a.txt').write_bytes(b'same mount\n')
    os.link(root/'parent/a.txt', root/'parent/alias.txt')
    (root/'parent/ordinary/alone.txt').write_bytes(b'unique contents')
    for name in ['source/b.txt', 'source/nested/c.txt']:
        (root/name).write_bytes(b'same mount\n')
    stage, native, entry = map(Path, sys.argv[3:6])
    stage.mkdir()
    tmp = stage/'folderwing-wsl-real.fresh'
    shutil.copyfile(native, tmp); tmp.chmod(0o755); tmp.replace(stage/'folderwing-wsl-real')
    (stage/'folderwing-wsl').write_text('#!/bin/sh\nexport OPENPILOT_MOUNT_ROOT='+shlex.quote(raw)+
                              '\nexec python3 '+shlex.quote(str(entry))+' "$@"\n')
    (stage/'folderwing-wsl').chmod(0o755)
    print(os.environ['WSL_DISTRO_NAME'])
elif action in ['check', 'cleanup']:
    if action == 'cleanup' and not root.exists(): sys.exit(0)
    assert not root.is_symlink() and root.stat().st_uid == os.getuid()
    assert (root/'.owner').read_text() == 'openpilot-duplicate-mount/v1'
    assert not os.path.ismount(root/'parent/mounted')
    assert not list((root/'parent/mounted').iterdir())
    traces = []
    for p in (root/'traces').iterdir():
        assert re.fullmatch(r'[0-9]+\.json', p.name) and not p.is_symlink()
        j = json.loads(p.read_text())
        assert j['outer'] != j['inner'] and j['same_device']
        assert j['parent_mount'] != j['target_mount']
        traces.append(j)
    if action == 'check':
        assert any(t['args'] == ['--analyze-space'] for t in traces)
        assert any(t['args'] == ['--duplicate-read'] for t in traces)
        print(json.dumps(dict(global_mount_unchanged=True, helpers=len(traces), traces=traces)))
    else:
        for p in (root/'traces').iterdir(): p.unlink()
        for name in ['parent/a.txt', 'parent/alias.txt', 'parent/ordinary/alone.txt',
                     'source/b.txt', 'source/nested/c.txt', '.owner']:
            (root/name).unlink()
        for name in ['parent/mounted', 'parent/ordinary', 'parent', 'source/nested', 'source', 'traces']:
            (root/name).rmdir()
        root.rmdir()
else:
    raise ValueError(action)
