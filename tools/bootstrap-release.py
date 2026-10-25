"""Restore checksum-pinned inputs into a checkout, without any user-level cache."""
from __future__ import annotations
import argparse, hashlib, json, os, shutil, stat, subprocess, tarfile, tempfile, urllib.request, zipfile
from pathlib import Path, PurePosixPath

ROOT=Path(__file__).resolve().parents[1]

def sha(path):
    h=hashlib.sha256()
    with path.open('rb') as f:
        for block in iter(lambda:f.read(1024*1024),b''):h.update(block)
    return h.hexdigest()

def read(path):return json.loads(path.read_text(encoding='utf-8-sig'))

def tree(path):
    return {str(p.relative_to(path)).replace('\\','/'):sha(p) for p in sorted(path.rglob('*')) if p.is_file() and p.name!='.folderwing-bootstrap.json'}

def download(pin, cache):
    if not pin.get('url'):raise ValueError('Supply the pinned local PDFium archive or FOLDERWING_PDFIUM_BASE_URL; no upstream fallback is used.')
    name=pin['url'].rsplit('/',1)[1];target=cache/name
    if target.exists():
        if sha(target)!=pin['sha256']:raise ValueError('Cached archive checksum mismatch: '+name)
        return target
    cache.mkdir(parents=True,exist_ok=True)
    fd,temp=tempfile.mkstemp(prefix='download-',suffix='.part',dir=cache);os.close(fd);temp=Path(temp)
    try:
        print('Download',name,flush=True)
        req=urllib.request.Request(pin['url'],headers={'User-Agent':'Folderwing-pinned-bootstrap/1'})
        if os.name=='nt':
            # Use Windows' system trust store through the built-in downloader.
            # TLS remains verified; the archive hash is checked independently.
            subprocess.run(['curl.exe','--fail','--silent','--show-error','--location','--proto','=https','--proto-redir','=https','--max-time','120','--retry','0','--output',str(temp),pin['url']],check=True)
        else:
            with urllib.request.urlopen(req,timeout=120) as response,temp.open('wb') as out:shutil.copyfileobj(response,out)
        if sha(temp)!=pin['sha256']:raise ValueError('Download checksum mismatch: '+name)
        if 'bytes' in pin and temp.stat().st_size!=pin['bytes']:raise ValueError('Download size mismatch: '+name)
        temp.replace(target)
    finally:temp.unlink(missing_ok=True)
    return target

def safe_name(name):
    name=name.replace('\\','/')
    p=PurePosixPath(name)
    if not name or p.is_absolute() or '..' in p.parts or ':' in name or '\x00' in name:raise ValueError('Unsafe archive path: '+name)
    return p

def extract(archive,directory):
    if zipfile.is_zipfile(archive):
        with zipfile.ZipFile(archive) as z:
            seen=set()
            for e in z.infolist():
                n=safe_name(e.filename);key=str(n).casefold()
                if key in seen:raise ValueError('Duplicate archive path: '+str(n))
                seen.add(key)
                if stat.S_ISLNK(e.external_attr>>16):raise ValueError('Archive symlink refused: '+str(n))
                target=directory.joinpath(*n.parts)
                if e.is_dir():target.mkdir(parents=True,exist_ok=True)
                else:
                    target.parent.mkdir(parents=True,exist_ok=True)
                    with z.open(e) as source,target.open('xb') as out:shutil.copyfileobj(source,out)
    else:
        with tarfile.open(archive) as t:
            for e in t.getmembers():
                safe_name(e.name)
                if not(e.isfile() or e.isdir()):raise ValueError('Archive link/device refused: '+e.name)
            t.extractall(directory,filter='data')

def install(name,pin,cache,archive_override=None):
    destination=ROOT/'third_party'/name
    if destination.exists():
        stamp=destination/'.folderwing-bootstrap.json'
        if not stamp.exists():raise ValueError('Existing unowned dependency retained; use a fresh checkout: '+name)
        old=read(stamp)
        if old['archive_sha256']!=pin['sha256'] or tree(destination)!=old['files']:raise ValueError('Dependency cache drift: '+name)
        print('Verified',name,flush=True);return old
    archive=archive_override or download(pin,cache)
    if sha(archive)!=pin['sha256'] or ('bytes' in pin and archive.stat().st_size!=pin['bytes']):raise ValueError('Dependency archive checksum/size mismatch: '+name)
    destination.parent.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=name+'-',dir=destination.parent) as temp:
        stage=Path(temp);extract(archive,stage);source=stage/pin.get('root','')
        for target,original in pin.get('aliases',{}).items():
            source_name=safe_name(original);target_name=safe_name(target)
            src_file=source.joinpath(*source_name.parts);target_file=source.joinpath(*target_name.parts)
            if not src_file.is_file() or target_file.exists():raise ValueError('Invalid dependency alias: '+target)
            target_file.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(src_file,target_file)
        for item in pin.get('required',[]):
            if not (source/item).is_file():raise ValueError('Missing dependency input: '+name+'/'+item)
        for item,digest in pin.get('source_sha256',{}).items():
            if sha(source/item)!=digest:raise ValueError('Dependency source changed: '+item)
        if pin.get('stamp'):(source/pin['stamp']).write_text(pin['version']+'\n')
        receipt={'archive_sha256':pin['sha256'],'version':pin.get('version'),'files':tree(source)}
        (source/'.folderwing-bootstrap.json').write_text(json.dumps(receipt,sort_keys=True)+'\n')
        # Never replace an existing dependency, including a concurrent creator.
        if destination.exists():raise ValueError('Dependency appeared during bootstrap: '+name)
        source.rename(destination)
    print('Restored',name,flush=True);return receipt

def media(archive,pin):
    dest=ROOT/'third_party/mpv'
    def verify(path):return path.is_dir() and all((path/n).is_file() and sha(path/n)==v['sha256'] for n,v in pin['files'].items())
    if dest.exists():
        if not verify(dest):raise ValueError('Existing media input mismatch; retained')
        return
    if not archive or not archive.is_file() or sha(archive)!=pin['archive_sha256']:raise ValueError('Missing or incorrect pinned media ZIP')
    with tempfile.TemporaryDirectory(prefix='mpv-',dir=dest.parent) as temp:
        stage=Path(temp)/'data';stage.mkdir();extract(archive,stage)
        if {str(p.relative_to(stage)).replace('\\','/') for p in stage.rglob('*') if p.is_file()}!=set(pin['files']):raise ValueError('Unexpected media archive entries')
        if not verify(stage):raise ValueError('Media content mismatch')
        if dest.exists():raise ValueError('Media destination appeared during bootstrap')
        stage.rename(dest)

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--media-archive',type=Path);p.add_argument('--media-base-url');p.add_argument('--media-source-archive',type=Path);p.add_argument('--pdfium-archive',type=Path);p.add_argument('--pdfium-recipe-archive',type=Path);p.add_argument('--pdfium-base-url');p.add_argument('--cache',type=Path);p.add_argument('--with-toolchain-source',action='store_true');a=p.parse_args()
    if os.name!='nt':raise SystemExit('Run this Windows input bootstrap using Windows Python 3.12+; Linux media builds have a separate recipe.')
    cache=a.cache or ROOT/'build/downloads';lock=read(ROOT/'tools/release-inputs.json');mpv=read(ROOT/'tools/media-dependencies.json')
    archive=a.media_archive or (Path(os.environ['FOLDERWING_MEDIA_ARCHIVE']) if os.environ.get('FOLDERWING_MEDIA_ARCHIVE') else None)
    base=a.media_base_url or os.environ.get('FOLDERWING_MEDIA_BASE_URL')
    if archive is None and base:
        archive=download({'url':base.rstrip('/')+'/'+mpv['archive_name'],'sha256':mpv['archive_sha256']},cache)
    if archive is None or not archive.is_file() or sha(archive)!=mpv['archive_sha256']:raise SystemExit('Supply --media-archive with the accepted ZIP or --media-base-url with its hosting directory. No unpinned fallback is used.')
    source_archive=a.media_source_archive or (Path(os.environ['FOLDERWING_MEDIA_SOURCE_ARCHIVE']) if os.environ.get('FOLDERWING_MEDIA_SOURCE_ARCHIVE') else None)
    if source_archive is None and base:source_archive=download({'url':base.rstrip('/')+'/'+mpv['source_bundle']['filename'],'sha256':mpv['source_bundle']['sha256']},cache)
    if source_archive is None or not source_archive.is_file() or sha(source_archive)!=mpv['source_bundle']['sha256']:raise SystemExit('Supply the exact corresponding sources via --media-source-archive or the media hosting directory.')
    cache.mkdir(parents=True,exist_ok=True)
    cached_source=cache/mpv['source_bundle']['filename']
    if cached_source.exists():
        if sha(cached_source)!=mpv['source_bundle']['sha256']:raise ValueError('Cached corresponding-source mismatch')
    elif source_archive.resolve()!=cached_source.resolve():shutil.copyfile(source_archive,cached_source)
    pdfpin=dict(lock['dependencies']['pdfium']);pdfbase=a.pdfium_base_url or os.environ.get('FOLDERWING_PDFIUM_BASE_URL')
    if pdfbase:pdfpin['url']=pdfbase.rstrip('/')+'/'+pdfpin['archive_name']
    pdfarchive=a.pdfium_archive or (Path(os.environ['FOLDERWING_PDFIUM_ARCHIVE']) if os.environ.get('FOLDERWING_PDFIUM_ARCHIVE') else None)
    if pdfarchive is None:pdfarchive=download(pdfpin,cache)
    if not pdfarchive.is_file() or sha(pdfarchive)!=pdfpin['sha256'] or pdfarchive.stat().st_size!=pdfpin['bytes']:raise ValueError('PDFium archive checksum/size mismatch')
    recipe=pdfpin['recipe_bundle'];pdfrecipe=a.pdfium_recipe_archive or (Path(os.environ['FOLDERWING_PDFIUM_RECIPE_ARCHIVE']) if os.environ.get('FOLDERWING_PDFIUM_RECIPE_ARCHIVE') else None)
    if pdfrecipe is None and pdfbase:pdfrecipe=download(dict(recipe,url=pdfbase.rstrip('/')+'/'+recipe['filename']),cache)
    if pdfrecipe is None or not pdfrecipe.is_file() or sha(pdfrecipe)!=recipe['sha256'] or pdfrecipe.stat().st_size!=recipe['bytes']:raise ValueError('Supply the exact PDFium recipe ZIP via --pdfium-recipe-archive or FOLDERWING_PDFIUM_BASE_URL')
    cached_recipe=cache/recipe['filename']
    if cached_recipe.exists():
        if sha(cached_recipe)!=recipe['sha256']:raise ValueError('Cached PDFium recipe mismatch')
    elif pdfrecipe.resolve()!=cached_recipe.resolve():shutil.copyfile(pdfrecipe,cached_recipe)
    records={};zig=dict(lock['zig']);zig['required']=['zig.exe','LICENSE','lib/libc/mingw/COPYING','lib/libc/musl/COPYRIGHT'];records['zig']=install('zig',zig,cache)
    executable=ROOT/'third_party/zig/zig.exe'
    if sha(executable)!=zig['executable_sha256']:raise ValueError('Compiler executable does not match the accepted compiler')
    if subprocess.check_output([str(executable),'version'],text=True).strip()!=zig['version']:raise ValueError('Compiler version mismatch')
    if a.with_toolchain_source:records['zig-source']=install('zig-source',dict(zig['source'],version=zig['version']),cache)
    for name,pin in lock['dependencies'].items():records[name]=install(name,pin,cache,pdfarchive if name=='pdfium' else None)
    media(archive,mpv)
    receipt={'schema':'folderwing-bootstrap/v1','outcome':'passed','inputs_sha256':sha(ROOT/'tools/release-inputs.json'),'media_manifest_sha256':sha(ROOT/'tools/media-dependencies.json'),'compiler_sha256':sha(executable),'compiler_version':zig['version'],'dependencies':{n:{'archive_sha256':v['archive_sha256'],'tree_sha256':hashlib.sha256(json.dumps(v['files'],sort_keys=True,separators=(',',':')).encode()).hexdigest(),'file_count':len(v['files'])} for n,v in records.items()},'media_archive_sha256':mpv['archive_sha256'],'user_cache_used':False}
    output=ROOT/'build/bootstrap-receipt.json';output.parent.mkdir(parents=True,exist_ok=True);output.write_text(json.dumps(receipt,indent=2)+'\n');print('Pinned bootstrap complete; compiler:',executable,flush=True)
if __name__=='__main__':main()
