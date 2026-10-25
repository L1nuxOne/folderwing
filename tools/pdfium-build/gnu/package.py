"""Package the controlled GNU PDFium candidate and its complete runtime notices.

Produces a bootstrap-compatible ZIP and a source/recipe companion. Does not
install files or publish releases. Acceptance/adoption is a separate step.
"""
from __future__ import annotations
import argparse
import json
import shutil
import tempfile
import zipfile
from pathlib import Path
from audit_runtime import audit
from build import HERE, need, pinned, read, save, sha

ROOT = HERE.parents[2]

def zip_tree(folder, archive):
    with zipfile.ZipFile(archive, 'x', zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        for f in sorted(folder.rglob('*')):
            if not f.is_file():
                continue
            need(not f.is_symlink(), 'Symlink in package')
            item = zipfile.ZipInfo(f.relative_to(folder).as_posix(), date_time=(2026, 1, 1, 0, 0, 0))
            item.create_system = 3
            item.external_attr = 0o100644 << 16
            item.compress_type = zipfile.ZIP_DEFLATED
            z.writestr(item, f.read_bytes())

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--build-directory', type=Path, required=True)
    ap.add_argument('--source-root', type=Path, required=True)
    ap.add_argument('--toolchain-root', type=Path, required=True)
    ap.add_argument('--baseline', type=Path, required=True)
    ap.add_argument('--output-directory', type=Path, required=True)
    a = ap.parse_args()
    out = a.output_directory.resolve()
    need(not out.exists(), 'Use a new package directory')
    result = audit(a.build_directory, a.baseline, a.toolchain_root)
    pin = read(HERE / 'inputs.json')
    build = read(a.build_directory / 'BUILD.json')
    notice_rows = read(HERE / 'notices.json') + read(HERE / 'supplemental-notices.json')
    for row in notice_rows:
        pinned(HERE, row['path'], row['sha256'])
    need({p.relative_to(HERE).as_posix() for p in (HERE / 'licenses').rglob('*') if p.is_file()} == {r['path'] for r in notice_rows}, 'Uncatalogued GNU runtime notice')
    inventory = read(ROOT / 'tools/dependency-inventory.json')
    # Revalidate the existing PDFium component notices before reusing the
    # conservative upstream notice bundle. The replaced Chromium libc++ and
    # Microsoft runtime note are excluded from this GNU package.
    excluded = {'MICROSOFT-RUNTIME-NOTE.txt', 'libc++.LICENSE.TXT'}
    prefix = 'licenses/third-party/pdfium/'
    expected_notices = set()
    for row in inventory['notice_files']:
        name = row['path']
        if name.startswith(prefix) and name[len(prefix):] not in excluded:
            pinned(ROOT, name, row['sha256'])
            expected_notices.add(name[len(prefix):])
    need({p.relative_to(ROOT / prefix).as_posix() for p in (ROOT / prefix).rglob('*') if p.is_file() and p.relative_to(ROOT / prefix).as_posix() not in excluded} == expected_notices, 'Uncatalogued PDFium notice')
    out.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='pdfium-gnu-', dir=out.parent) as temporary:
        temp = Path(temporary)
        stage = temp / 'runtime'
        stage.mkdir()
        for source, name in [(a.build_directory / 'pdfium.dll', 'bin/pdfium.dll'), (a.build_directory / 'pdfium.dll.a', 'lib/pdfium.dll.a')]:
            dest = stage / name
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, dest)
        need({p.name for p in (a.source_root / 'public').glob('*.h')} == set(build['public_headers_sha256']), 'C header set changed')
        (stage / 'include').mkdir()
        for name, digest in build['public_headers_sha256'].items():
            header_root = a.build_directory / 'overlay/public' if name == 'fpdfview.h' else a.source_root / 'public'
            shutil.copyfile(pinned(header_root, name, digest), stage / 'include' / name)
        notices = stage / 'licenses/pdfium'
        shutil.copytree(ROOT / 'licenses/third-party/pdfium', notices, ignore=shutil.ignore_patterns(*excluded))
        shutil.copytree(HERE / 'licenses', stage / 'licenses/gnu-runtime')
        shutil.copyfile(HERE / 'RUNTIME.md', stage / 'RUNTIME.md')
        (stage / 'VERSION').write_text('MAJOR=157\nMINOR=0\nBUILD=8086\nPATCH=0\n')
        (stage / 'THIRD-PARTY-NOTICES.txt').write_text('PDFium chromium/8086, built for the Windows GNU ABI with LLVM-MinGW.\nPDFium and engine component notices: licenses/pdfium/.\nLLVM/compiler-rt/libc++/libc++abi/libunwind and MinGW/winpthreads: licenses/gnu-runtime/.\nRuntime provenance and conditions: RUNTIME.md. Folderwing MIT does not relicense these components.\nWindows UCRT is an OS prerequisite. No Visual C++ runtime redistributable is required or bundled.\nThis candidate still requires Folderwing acceptance and adoption before becoming the default.\n')
        info = {'schema': 'folderwing-pdfium-gnu-package/v1', 'source_commit': pin['source_commit'], 'version': pin['version'], 'inputs_sha256': sha(HERE / 'inputs.json'), 'build_receipt_sha256': sha(a.build_directory / 'BUILD.json'), 'runtime_audit': result, 'public_release_ready': False, 'files': {p.relative_to(stage).as_posix(): sha(p) for p in sorted(stage.rglob('*')) if p.is_file()}}
        save(stage / 'BUILDINFO.json', info)
        archive = temp / 'folderwing-pdfium-gnu-8086-win-x64.zip'
        zip_tree(stage, archive)
        # Retain the exact maintenance recipe and source revision graph. Upstream
        # permissive source repositories are referenced by immutable revision;
        # this companion is NOT mislabeled as a full PDFium source archive.
        companion = temp / 'recipe'
        shutil.copytree(HERE, companion / 'gnu', ignore=shutil.ignore_patterns('__pycache__', '*.pyc'))
        shutil.copytree(HERE.parent / 'patches', companion / 'patches')
        for name in ['source-graph.json', 'audit.py']:
            shutil.copyfile(HERE.parent / name, companion / name)
        shutil.copyfile(ROOT / 'LICENSE', companion / 'FOLDERWING-LICENSE')
        shutil.copyfile(a.build_directory / 'BUILD.json', companion / 'BUILD.json')
        shutil.copyfile(a.build_directory / 'runtime-members.tsv', companion / 'runtime-members.tsv')
        source_archive = temp / 'folderwing-pdfium-gnu-8086-recipe.zip'
        zip_tree(companion, source_archive)
        manifest = {'schema': 'folderwing-pdfium-gnu-artifacts/v1', 'runtime_sha256': result['dll_sha256'], 'artifacts': {p.name: {'sha256': sha(p), 'bytes': p.stat().st_size} for p in [archive, source_archive]}, 'public_release_ready': False, 'installed_release_adopted': False}
        shutil.rmtree(stage)
        shutil.rmtree(companion)
        save(temp / 'MANIFEST.json', manifest)
        (temp / 'SHA256SUMS').write_text(''.join(sha(p) + '  ' + p.name + '\n' for p in sorted(temp.iterdir()) if p.is_file()))
        need(not out.exists(), 'Output appeared while packaging')
        temp.rename(out)
    print(json.dumps(manifest, indent=2))

if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError, KeyError) as e:
        raise SystemExit(str(e))
