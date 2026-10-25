"""Build PDFium's frozen GNU action graph without Microsoft SDK or MSVC inputs.

Restore the upstream graph in ../README.md first. The frozen action graph replaces
GN at build time; it contains source paths and deduplicated flags, not objects.
All target objects and the build-only NASM executable are compiled afresh.
"""
from __future__ import annotations
import argparse
import concurrent.futures
import hashlib
import json
import os
import shutil
import subprocess
import time
from pathlib import Path
import sys
sys.path.append(str(Path(__file__).resolve().parents[1]))
from audit import need

HERE = Path(__file__).resolve().parent

def sha(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        for block in iter(lambda: f.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()

def read(path):
    return json.loads(path.read_text(encoding='utf-8-sig'))

def save(path, value):
    path.write_text(json.dumps(value, indent=2) + '\n', encoding='utf-8')

def safe(root, name):
    p = (root / name).resolve()
    need(p.is_relative_to(root.resolve()) and p != root.resolve(), 'Escaping input/output: ' + name)
    return p

def pinned(root, name, digest):
    p = safe(root, name)
    need(p.is_file() and not p.is_symlink() and sha(p) == digest, 'Pinned file mismatch: ' + str(p))
    return p

def git(git_exe, folder, *args):
    return subprocess.check_output([str(git_exe), '-c', 'core.safecrlf=false', '-C', str(folder), *args], text=True).strip()

def source_state(src, git_exe, pin):
    graph = read(pinned(HERE.parent, 'source-graph.json', pin['source_graph_sha256']))
    state = {}
    for name, row in graph.items():
        rev = row.get('rev') or ''
        if len(rev) != 40 or any(c not in '0123456789abcdef' for c in rev):
            continue  # CIPD build tools are not executed or linked by this recipe.
        folder = safe(src.parent, name)
        need(git(git_exe, folder, 'rev-parse', 'HEAD') == rev, 'Source revision changed: ' + name)
        diff = subprocess.check_output([str(git_exe), '-c', 'core.safecrlf=false', '-C', str(folder), 'diff', 'HEAD', '--no-ext-diff', '--no-color', '--full-index', '--binary', '--']).replace(b'\r\n', b'\n')
        expected = b''
        if folder == src:
            expected = (HERE.parent / 'patches/pdfium.patch').read_bytes().replace(b'\r\n', b'\n')
        elif folder == src / 'build':
            expected = (HERE.parent / 'patches/chromium-build.patch').read_bytes().replace(b'\r\n', b'\n')
        # Fresh upstream trees and the exact accepted interface/build patches are
        # both lawful. The portability patch is always isolated in the overlay.
        need(diff in (b'', expected), 'Unexpected source changes: ' + name)
        extras = git(git_exe, folder, 'ls-files', '--others', '--exclude-standard').splitlines()
        need(not any(Path(n).suffix.lower() in ('.h', '.hpp', '.inc', '.c', '.cc', '.cpp', '.asm') for n in extras), 'Untracked source/header input: ' + name)
        state[name] = {'revision': rev, 'diff_sha256': hashlib.sha256(diff).hexdigest()}
    return state

def clean_env():
    env = os.environ.copy()
    for n in ['INCLUDE', 'LIB', 'LIBPATH', 'CL', '_CL_', 'CPATH', 'C_INCLUDE_PATH', 'CPLUS_INCLUDE_PATH', 'CCC_OVERRIDE_OPTIONS', 'SDKROOT', 'COMPILER_PATH', 'GCC_EXEC_PREFIX', 'LIBRARY_PATH']:
        env.pop(n, None)
    return env

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--source-root', required=True, type=Path)
    ap.add_argument('--toolchain-root', required=True, type=Path)
    ap.add_argument('--git', required=True, type=Path)
    ap.add_argument('--output-directory', required=True, type=Path)
    ap.add_argument('--jobs', type=int, default=3)
    a = ap.parse_args()
    need(os.name == 'nt', 'Use Windows Python')
    need(1 <= a.jobs <= 8, 'Use 1–8 workers')
    src, tc, out = a.source_root.resolve(), a.toolchain_root.resolve(), a.output_directory.resolve()
    need(not out.exists(), 'Output directory must be new')
    need(not out.is_relative_to(src) and not src.is_relative_to(out), 'Build outside the source tree')
    pin = read(HERE / 'inputs.json')
    graph = read(pinned(HERE, 'graph.json', pin['graph_sha256']))
    sources = read(pinned(HERE, 'sources.json', pin['sources_sha256']))
    tc_files = read(pinned(HERE, 'toolchain-files.json', pin['toolchain_files_sha256']))
    pinned(HERE, 'notices.json', pin['notices_sha256'])
    patch = pinned(HERE.parent, 'patches/llvm-mingw-portability.patch', pin['patch_sha256'])
    interface_patch = pinned(HERE.parent, 'patches/pdfium.patch', pin['interface_patch_sha256'])
    for n, h in tc_files.items():
        pinned(tc, n, h)
    for n, h in sources.items():
        pinned(src, n, h)
    before = source_state(src, a.git, pin)
    started = time.monotonic()
    out.mkdir(parents=True)
    work = out / 'work'
    (work / 'gen').mkdir(parents=True)
    overlay = out / 'overlay'
    changed = ['core/fxcrt/fx_memory.cpp', 'core/fxcrt/fx_memory.h', 'core/fxge/win32/cgdi_display_driver.h', 'public/fpdfview.h']
    for name in changed + ['BUILD.gn']:
        dest = safe(overlay, name)
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes((src / name).read_bytes().replace(b'\r\n', b'\n'))
    subprocess.run([str(a.git), 'apply', '--check', str(patch)], cwd=overlay, check=True)
    subprocess.run([str(a.git), 'apply', str(patch)], cwd=overlay, check=True)
    already_patched = subprocess.run([str(a.git), 'apply', '--reverse', '--check', str(interface_patch)], cwd=overlay, capture_output=True).returncode == 0
    if not already_patched:
        subprocess.run([str(a.git), 'apply', '--check', str(interface_patch)], cwd=overlay, check=True)
        subprocess.run([str(a.git), 'apply', str(interface_patch)], cwd=overlay, check=True)
    vfs = {'version': 0, 'roots': [{'type': 'file', 'name': str(src / n), 'external-contents': str(overlay / n)} for n in changed]}
    save(out / 'overlay.json', vfs)
    source_headers = {p.name: sha(p) for p in sorted((src / 'public').glob('*.h'))}
    headers = dict(source_headers)
    headers['fpdfview.h'] = sha(overlay / 'public/fpdfview.h')
    cc = tc / 'bin/x86_64-w64-mingw32-clang++.exe'
    def expand(s):
        return s.replace('@SRC@', src.as_posix()).replace('@OUT@', out.as_posix())
    def command(u):
        dest = safe(out / 'objects', u['output'])
        dest.parent.mkdir(parents=True, exist_ok=True)
        source = safe(src, u['source'])
        if u in graph['assembly']:
            return [str(out / 'nasm.exe'), *[expand(s) for s in u['flags']], '-o', str(dest), str(source)]
        flags = [expand(s) for s in graph['flags'][u['flags']]]
        language = 'c' if source.suffix == '.c' else 'c++'
        return [str(cc), '-x', language, '-c', str(source), '-o', str(dest), '-ivfsoverlay', str(out / 'overlay.json'), '-ffile-prefix-map=' + src.as_posix() + '=.', '-ffile-prefix-map=' + out.as_posix() + '=.', *flags]
    units = graph['units'] + graph['assembly']
    actions = [{'source': u['source'], 'output': u['output'], 'command': command(u)} for u in units]
    input_record = {'schema': 'folderwing-pdfium-gnu-build-input/v1', 'recipe_sha256': sha(Path(__file__)), 'inputs_sha256': sha(HERE / 'inputs.json'), 'source_state': before, 'sources_sha256': sources, 'source_headers_sha256': source_headers, 'public_headers_sha256': headers, 'toolchain_files_sha256': tc_files, 'overlay_sha256': {n: sha(overlay / n) for n in changed}, 'actions': actions, 'workers': a.jobs, 'profile': 'Windows GNU x64 / UCRT; no interactive desktop; no WSL control', 'public_release_ready': False}
    save(out / 'INPUT.json', input_record)
    logs = out / 'logs'
    logs.mkdir()
    env = clean_env()
    results = []
    def execute(cmd, name):
        begin = time.monotonic()
        with (logs / name).open('wb') as f:
            p = subprocess.run(cmd, cwd=work, env=env, stdout=f, stderr=subprocess.STDOUT, timeout=180)
        need(p.returncode == 0, 'Build command failed; see ' + str(logs / name))
        return {'log': name, 'seconds': round(time.monotonic() - begin, 3), 'exit_code': p.returncode}
    def compile_group(rows, label):
        with concurrent.futures.ThreadPoolExecutor(max_workers=a.jobs) as pool:
            futures = [pool.submit(execute, command(u), u['output'].replace('/', '_') + '.log') for u in rows]
            for i, f in enumerate(futures):
                results.append(f.result())
                if (i + 1) % 100 == 0:
                    print(label, i + 1, '/', len(rows), flush=True)
    def link(rows, output, shared):
        rsp = out / (output + '.rsp')
        rsp.write_text('\n'.join('"' + safe(out / 'objects', u['output']).as_posix() + '"' for u in rows) + '\n')
        cmd = [str(cc), '-v', '-static', '-mguard=cf', '-fstack-protector-strong', '-o', str(out / output), '@' + str(rsp), '-Wl,--dynamicbase', '-Wl,--nxcompat', '-Wl,--high-entropy-va', '-Wl,--no-insert-timestamp', '-Wl,--Xlink=/cetcompat', '-Wl,--gc-sections', '-Wl,--icf=all', '-Wl,--strip-debug']
        if shared:
            cmd += ['-shared', '-Wl,--exclude-all-symbols', '-Wl,--Map=' + str(out / 'pdfium.map'), '-Wl,--out-implib,' + str(out / 'pdfium.dll.a'), '-Wl,--Xlink=/verbose', '-Wl,--Xlink=/reproduce:' + str(out / 'runtime-link-inputs.tar')]
            cmd += ['-l' + s for s in graph['system_libraries']]
        save(out / (output + '-LINK-INPUT.json'), {'command': cmd, 'objects': {u['output']: sha(safe(out / 'objects', u['output'])) for u in rows}})
        results.append(execute(cmd, output + '-link.log'))
        if shared:
            import csv
            import re
            rows = []
            for archive, member, symbol in re.findall(r'^ld\.lld: Loaded (.+\.a)\(([^\n]+)\) for ([^\n]+)', (logs / (output + '-link.log')).read_text(errors='replace'), re.MULTILINE):
                name = Path(archive).name
                matches = [p for p in [tc / 'x86_64-w64-mingw32/lib' / name, tc / 'lib/clang/23/lib/windows' / name, tc / 'lib' / name] if p.is_file()]
                need(len(matches) == 1, 'Ambiguous runtime archive: ' + archive)
                rows.append([str(matches[0]), member, symbol.rstrip()])
            need(rows, 'No linker runtime extraction trace')
            with (out / 'runtime-members.tsv').open('w', newline='', encoding='utf-8') as f:
                writer = csv.writer(f, delimiter='\t')
                writer.writerow(['archive', 'member', 'symbol'])
                writer.writerows(rows)
    host = [u for u in graph['units'] if u['kind'] == 'host']
    target = [u for u in graph['units'] if u['kind'] == 'target']
    compile_group(host, 'NASM host')
    link(host, 'nasm.exe', False)
    compile_group(target, 'PDFium')
    compile_group(graph['assembly'], 'Assembly')
    link(target + graph['assembly'], 'pdfium.dll', True)
    need(source_state(src, a.git, pin) == before, 'Source tree changed during build')
    for n, h in sources.items():
        pinned(src, n, h)
    for n, h in tc_files.items():
        pinned(tc, n, h)
    for n, h in source_headers.items():
        pinned(src / 'public', n, h)
    for n, h in input_record['overlay_sha256'].items():
        pinned(overlay, n, h)
    need(sha(Path(__file__)) == input_record['recipe_sha256'] and sha(HERE / 'inputs.json') == input_record['inputs_sha256'], 'Recipe/pins changed during build')
    result = {'schema': 'folderwing-pdfium-gnu-build/v1', 'outcome': 'built-candidate', 'input_sha256': sha(out / 'INPUT.json'), 'recipe_sha256': input_record['recipe_sha256'], 'inputs_sha256': input_record['inputs_sha256'], 'source_commit': pin['source_commit'], 'public_headers_sha256': headers, 'counts': graph['counts'], 'workers': a.jobs, 'elapsed_seconds': round(time.monotonic() - started, 3), 'commands': results, 'files': {n: sha(out / n) for n in ['pdfium.dll', 'pdfium.dll.a', 'pdfium.map', 'runtime-members.tsv', 'runtime-link-inputs.tar', 'nasm.exe']}, 'microsoft_build_tools_used': False, 'public_release_ready': False}
    save(out / 'BUILD.json', result)
    print(json.dumps({k: v for k, v in result.items() if k not in ('commands', 'public_headers_sha256')}, indent=2))

if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError, subprocess.SubprocessError) as e:
        raise SystemExit(str(e))
