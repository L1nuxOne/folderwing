"""Restore pinned source repositories and LLVM-MinGW into a NEW owned workspace.

No hooks, package managers, Microsoft tools, registration, or global PATH edits.
An optional source cache supplies Git objects; committed files are checked out
afresh, so no generated or experimental build files are borrowed from it.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import re
import subprocess
import urllib.request
import zipfile
from pathlib import Path
from build import HERE, need, pinned, read, safe, save, sha

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--workspace', type=Path, required=True)
    ap.add_argument('--git', type=Path, required=True)
    ap.add_argument('--source-cache', type=Path)
    ap.add_argument('--toolchain-archive', type=Path)
    a = ap.parse_args()
    out = a.workspace.resolve()
    need(not out.exists(), 'Restore into a new workspace')
    pin = read(HERE / 'inputs.json')
    graph = read(pinned(HERE.parent, 'source-graph.json', pin['source_graph_sha256']))
    out.mkdir(parents=True)
    (out / '.folderwing-owned-pdfium-gnu').write_text('Pinned source/toolchain restore; no global changes\n')
    save(out / 'RESTORE-INPUT.json', {'recipe_sha256': sha(Path(__file__)), 'inputs_sha256': sha(HERE / 'inputs.json'), 'source_graph_sha256': pin['source_graph_sha256'], 'toolchain': pin['toolchain']})
    def git(*args):
        subprocess.run([str(a.git), '-c', 'core.autocrlf=true', '-c', 'core.hooksPath=' + str(out / 'no-hooks'), *map(str, args)], check=True)
    for name, row in graph.items():
        rev = row.get('rev') or ''
        if not re.fullmatch('[0-9a-f]{40}', rev):
            continue
        folder = safe(out, name)
        need(not folder.exists(), 'Unexpected existing dependency directory: ' + name)
        cache = a.source_cache / name if a.source_cache else None
        if cache and (cache / '.git').exists():
            git('clone', '--shared', '--no-checkout', cache, folder)
            git('-C', folder, 'remote', 'set-url', 'origin', row['url'])
            # Detach from the cache before completing restoration.
            git('-C', folder, 'repack', '-a', '-d')
            alternates = folder / '.git/objects/info/alternates'
            if alternates.exists():
                alternates.unlink()
        else:
            git('init', folder)
            git('-C', folder, 'remote', 'add', 'origin', row['url'])
            git('-C', folder, 'fetch', '--depth=1', 'origin', rev)
        git('-C', folder, 'config', 'core.autocrlf', 'true')
        git('-C', folder, 'checkout', '--detach', rev)
    archive = a.toolchain_archive
    if archive is None:
        archive = out / pin['toolchain']['asset']
        with urllib.request.urlopen(pin['toolchain']['url'], timeout=60) as source, archive.open('xb') as dest:
            while block := source.read(1024 * 1024):
                dest.write(block)
    need(archive.stat().st_size == pin['toolchain']['bytes'] and sha(archive) == pin['toolchain']['sha256'], 'Toolchain archive pin mismatch')
    tc = out / 'toolchain'
    tc.mkdir()
    root = pin['toolchain']['asset'][:-4] + '/'
    with zipfile.ZipFile(archive) as z:
        seen = set()
        for item in z.infolist():
            need(item.filename.startswith(root), 'Unexpected ZIP root')
            name = item.filename[len(root):]
            if not name:
                continue
            need('\\' not in name and ':' not in name and not name.startswith('/') and '..' not in Path(name).parts, 'Unsafe ZIP path')
            need((item.external_attr >> 16) & 0o170000 != 0o120000, 'ZIP symlink refused')
            need(name.lower() not in seen, 'Duplicate ZIP member')
            seen.add(name.lower())
            target = safe(tc, name)
            if item.is_dir():
                target.mkdir(parents=True, exist_ok=True)
            else:
                target.parent.mkdir(parents=True, exist_ok=True)
                with z.open(item) as source, target.open('xb') as dest:
                    while block := source.read(1024 * 1024):
                        dest.write(block)
    files = read(pinned(HERE, 'toolchain-files.json', pin['toolchain_files_sha256']))
    for n, h in files.items():
        pinned(tc, n, h)
    save(out / 'RESTORE.json', {'outcome': 'restored', 'input_sha256': sha(out / 'RESTORE-INPUT.json'), 'source_root': str(out / 'pdfium'), 'toolchain_root': str(tc), 'toolchain_sha256': pin['toolchain']['sha256'], 'microsoft_tools_required': False})

if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError, subprocess.SubprocessError) as e:
        raise SystemExit(str(e))
