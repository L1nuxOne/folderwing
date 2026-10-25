"""Check the GNU candidate's exact API, PE mitigations and extracted runtimes."""
from __future__ import annotations
import argparse
import csv
import json
import re
import struct
import tarfile
import sys
from pathlib import Path
sys.path.append(str(Path(__file__).resolve().parents[1]))
from audit import PE, need
from build import HERE, pinned, read, save, sha

def inspect(dll, baseline):
    pe = PE(dll)
    old = PE(baseline)
    need(pe.machine == 0x8664, 'Expected x64 PE')
    need(pe.exports() == old.exports() and len(pe.exports()) == 471, 'PDFium API set changed')
    need(pe.flags & 0x40 and pe.flags & 0x100 and pe.flags & 0x4000 and pe.flags & 0x20, 'ASLR/NX/CFG/high-entropy VA missing')
    imports = pe.imports()
    allowed = {'advapi32.dll', 'gdi32.dll', 'kernel32.dll', 'user32.dll', 'bcrypt.dll', 'dbghelp.dll', 'ucrtbase.dll'}
    for name in imports:
        need(name.lower() in allowed or name.lower().startswith('api-ms-win-crt-'), 'Unexpected runtime import: ' + name)
    need(any(n.lower().startswith('api-ms-win-crt-') or n.lower() == 'ucrtbase.dll' for n in imports), 'UCRT dependency missing')
    config_rva, size = pe.dirs[10]
    need(config_rva and size >= 148, 'Load configuration missing')
    config = pe.offset(config_rva)
    count = struct.unpack_from('<Q', pe.data, config + 136)[0]
    table = struct.unpack_from('<Q', pe.data, config + 128)[0]
    guard_flags = pe.u32(config + 144)
    need(table and count and guard_flags & 0x500 == 0x500, 'CFG table/instrumentation missing')
    rva, size = pe.dirs[6]
    need(rva and size % 28 == 0, 'PE debug directory missing')
    off = pe.offset(rva)
    cet = False
    for i in range(size // 28):
        row = struct.unpack_from('<IIHHIIII', pe.data, off + i * 28)
        if row[4] == 20 and row[5] >= 4:
            cet |= bool(pe.u32(row[7]) & 1)
    need(cet, 'CET compatibility marker missing after release link')
    return {'dll_sha256': sha(dll), 'bytes': dll.stat().st_size, 'export_count': len(pe.exports()), 'imports': imports, 'hardening': {'aslr': True, 'nx': True, 'high_entropy_va': True, 'cfg_instrumented': True, 'cfg_table_entries': count, 'cet_compat': True}}

def audit(build, baseline, toolchain):
    pin = read(HERE / 'inputs.json')
    receipt = read(build / 'BUILD.json')
    need(receipt['outcome'] == 'built-candidate', 'Build did not complete')
    need(receipt['recipe_sha256'] == sha(HERE / 'build.py') and receipt['inputs_sha256'] == sha(HERE / 'inputs.json'), 'Recipe/pins changed since build')
    for name, digest in receipt['files'].items():
        pinned(build, name, digest)
    input_record = read(pinned(build, 'INPUT.json', receipt['input_sha256']))
    for name, digest in input_record['toolchain_files_sha256'].items():
        pinned(toolchain, name, digest)
    result = inspect(build / 'pdfium.dll', baseline)
    runtime = {}
    allowed = {'libc++.a': 'LLVM libc++/libc++abi', 'libunwind.a': 'LLVM libunwind', 'libclang_rt.builtins-x86_64.a': 'LLVM compiler-rt', 'libwinpthread.a': 'MinGW winpthreads', 'libmingw32.a': 'MinGW startup/runtime', 'libmingwex.a': 'MinGW extended runtime', 'libmoldname.a': 'MinGW compatibility imports', 'libmsvcrt.a': 'MinGW UCRT imports and compatibility implementation'}
    graph = read(HERE / 'graph.json')
    for n in graph['system_libraries']:
        allowed.setdefault('lib' + n + '.a', 'MinGW Windows system imports')
    text = (build / 'runtime-members.tsv').read_text(encoding='utf-8-sig')
    for row in csv.DictReader(text.splitlines(), delimiter='\t'):
        archive = Path(row['archive']).resolve()
        need(archive.is_relative_to(toolchain.resolve()) and archive.name in allowed, 'Unexpected extracted runtime archive: ' + str(archive))
        runtime.setdefault(archive.name, {'component': allowed[archive.name], 'sha256': sha(archive), 'members': []})['members'].append(row['member'])
    need({'libc++.a', 'libwinpthread.a', 'libmingw32.a', 'libmingwex.a'} <= runtime.keys(), 'Expected runtime extraction evidence missing')
    provenance = read(HERE / 'runtime-sources.json')
    need(provenance['source_revision'] == pin['mingw_w64_commit'], 'Runtime source inventory revision differs')
    for name in ['libmingw32.a', 'libmingwex.a', 'libwinpthread.a', 'libmsvcrt.a']:
        for member in runtime[name]['members']:
            if member.lower().endswith('.dll'):
                continue  # Generated Windows/UCRT import member, no static body.
            need(name + '(' + member + ')' in provenance['members'], 'Unmapped MinGW implementation member: ' + name + '(' + member + ')')
    mapping = (build / 'pdfium.map').read_text(errors='replace')
    need('__stack_chk_fail' in mapping and '__stack_chk_guard' in mapping, 'GNU stack protector runtime missing')
    need(all('-fstack-protector-strong' in row['command'] for row in input_record['actions'] if '-c' in row['command']), 'Stack protection flag missing')
    result['hardening']['gnu_stack_protector'] = True
    startup = {}
    for name in ['dllcrt2.o', 'crtbegin.o', 'crtend.o']:
        path = toolchain / 'x86_64-w64-mingw32/lib' / name
        need(name in mapping, 'GNU startup object missing from map: ' + name)
        startup[name] = sha(path)
    # COFF verbose output names archives by basename. Bind those names to the
    # actual bytes captured by LLD's link reproduction archive, not inference
    # from an installed library with a similar filename.
    with tarfile.open(build / 'runtime-link-inputs.tar') as t:
        members = t.getmembers()
        for name, digest in {**{n: r['sha256'] for n, r in runtime.items()}, **startup}.items():
            matches = [m for m in members if m.isfile() and m.name.endswith('/' + name)]
            need(len(matches) == 1, 'Link reproduction input ambiguous/missing: ' + name)
            import hashlib
            stream = t.extractfile(matches[0])
            need(stream is not None and hashlib.file_digest(stream, 'sha256').hexdigest() == digest, 'Actual linked runtime archive differs: ' + name)
    need(not re.search(r'(?i)(?:msvcrt|vcruntime|libcmt|libcpmt|msvcprt)\.lib', mapping), 'Microsoft startup archive in map')
    # These driver-requested archives are intentionally empty, not GPL libssp.
    for name in ['libssp.a', 'libssp_nonshared.a']:
        need((toolchain / 'x86_64-w64-mingw32/lib' / name).read_bytes() == b'!<arch>\n', 'Unexpected libssp implementation code')
    result.update(schema='folderwing-pdfium-gnu-runtime-audit/v1', runtime_archives=runtime, startup_objects_sha256=startup, empty_ssp_stubs_verified=True, microsoft_startup_archives_observed=False, public_release_ready=False)
    return result

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--build-directory', required=True, type=Path)
    ap.add_argument('--baseline', required=True, type=Path)
    ap.add_argument('--toolchain-root', required=True, type=Path)
    ap.add_argument('--output', required=True, type=Path)
    a = ap.parse_args()
    need(not a.output.exists(), 'Audit output must be new')
    result = audit(a.build_directory, a.baseline, a.toolchain_root)
    save(a.output, result)
    print(json.dumps({k: v for k, v in result.items() if k not in ('runtime_archives', 'imports')}, indent=2))

if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError, KeyError, struct.error) as e:
        raise SystemExit(str(e))
