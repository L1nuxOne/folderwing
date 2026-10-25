"""Shared, standard-library-only release input and provenance checks."""
from pathlib import Path
import hashlib,json,re,subprocess
ROOT=Path(__file__).resolve().parents[1]
SOURCE_DIRS=('app','src','include','linux','tools','tests','docs','licenses','.github')
SOURCE_FILES=('README.md','LICENSE','.gitignore','.gitattributes','build.ps1')

def need(condition,message):
    if not condition:raise ValueError(message)

def read(path):return json.loads(path.read_text(encoding='utf-8-sig'))

def digest(value):return hashlib.sha256(json.dumps(value,sort_keys=True,separators=(',',':')).encode()).hexdigest()

def sha(path):
    h=hashlib.sha256()
    with path.open('rb') as f:
        for data in iter(lambda:f.read(1024*1024),b''):h.update(data)
    return h.hexdigest()

def plain(path):
    need(path.is_file() and not path.is_symlink(),'Missing or linked file: '+str(path))
    for parent in [path,*path.parents]:
        need(not parent.is_symlink() and not (hasattr(parent,'is_junction') and parent.is_junction()),'Linked source path refused: '+str(path))
    return path

def source_files():
    files=[ROOT/n for n in SOURCE_FILES if (ROOT/n).is_file()]
    for name in SOURCE_DIRS:
        files.extend(p for p in (ROOT/name).rglob('*') if p.is_file() and '__pycache__' not in p.parts and p.suffix.lower() not in ('.pyc','.pyo','.orig','.old'))
    return sorted(set(files))

def source_snapshot():return {p.relative_to(ROOT).as_posix():sha(plain(p)) for p in source_files()}

def version():
    match=re.search(r'^#define FW_VERSION "([0-9]+\.[0-9]+\.[0-9]+)"', (ROOT/'app/brand.h').read_text(),re.M)
    need(match,'Missing product version');return match.group(1)

def revision():
    try:return subprocess.check_output(['git','-C',str(ROOT),'rev-parse','HEAD'],text=True,stderr=subprocess.DEVNULL).strip()
    except (OSError,subprocess.CalledProcessError):return None

def seal_build(build,compiler):
    spec=read(ROOT/'tools/release-files.json');lock=read(ROOT/'tools/release-inputs.json');mpv=read(ROOT/spec['media_manifest'])
    need(sha(plain(compiler))==lock['zig']['executable_sha256'],'Compiler mismatch')
    binaries={n:sha(plain(build/n)) for n in spec['binaries']}
    need(binaries['libmpv-2.dll']==mpv['runtime_sha256'],'Media DLL mismatch')
    need(binaries['pdfium.dll']==sha(plain(ROOT/'third_party/pdfium/bin/pdfium.dll')),'PDFium DLL does not match restored input')
    need(binaries['pdfium.dll']==lock['dependencies']['pdfium']['source_sha256']['bin/pdfium.dll'],'PDFium DLL does not match the accepted GNU runtime pin')
    # Release bootstrap must precede build; the same pinned roots are used.
    bootstrap=read(ROOT/'build/bootstrap-receipt.json');need(bootstrap['outcome']=='passed','Bootstrap not accepted')
    need(bootstrap['inputs_sha256']==sha(ROOT/'tools/release-inputs.json'),'Bootstrap inputs changed')
    snapshot=source_snapshot()
    result={'schema':'folderwing-release-build/v1','outcome':'built','version':version(),'platform':'windows-x64','revision':revision(),'source_digest':digest(snapshot),'source_files':snapshot,'binaries':binaries,'compiler_sha256':sha(compiler),'bootstrap_sha256':sha(ROOT/'build/bootstrap-receipt.json'),'release_files_sha256':sha(ROOT/'tools/release-files.json'),'media_manifest_sha256':sha(ROOT/spec['media_manifest'])}
    (build/'BUILD-RECEIPT.json').write_text(json.dumps(result,indent=2)+'\n');return result
