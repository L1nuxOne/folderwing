"""Package a verified build using an explicit file list. Never publishes."""
from __future__ import annotations
import argparse,json,os,shutil,tempfile,zipfile
from pathlib import Path
from release_common import ROOT,need,read,sha,plain,digest,source_snapshot,version

def package(build,source_archive,output,publish_ready=False):
    spec=read(ROOT/'tools/release-files.json');inventory=read(ROOT/spec['notice_inventory']);mpv=read(ROOT/spec['media_manifest']);receipt=read(plain(build/'BUILD-RECEIPT.json'))
    need(receipt['outcome']=='built' and receipt['version']==version(),'Build receipt/version mismatch')
    need(receipt['source_digest']==digest(source_snapshot()),'Source inputs changed since build')
    need(receipt['release_files_sha256']==sha(ROOT/'tools/release-files.json'),'Release file list changed since build')
    need(receipt['media_manifest_sha256']==sha(ROOT/spec['media_manifest']),'Media input pin changed since build')
    need(inventory['project_license']=='MIT','Unexpected project license')
    need(sha(plain(ROOT/'LICENSE'))==inventory['project_license_sha256'],'Project license changed')
    if publish_ready:need(inventory['release_ready'] is True,'Public-release dependency closure remains open; inspect docs/DEPENDENCIES.md')
    need(source_archive.name==mpv['source_bundle']['filename'],'Corresponding-source filename mismatch')
    need(sha(plain(source_archive))==mpv['source_bundle']['sha256'],'Corresponding-source checksum mismatch')
    need(source_archive.stat().st_size==mpv['source_bundle']['bytes'],'Corresponding-source size mismatch')
    pdfpin=read(ROOT/'tools/release-inputs.json')['dependencies']['pdfium']
    recipe=pdfpin['recipe_bundle'];recipe_archive=ROOT/'build/downloads'/recipe['filename']
    need(sha(plain(recipe_archive))==recipe['sha256'] and recipe_archive.stat().st_size==recipe['bytes'],'PDFium recipe archive mismatch')
    files={}
    def add(name,path,expected=None):
        need(name not in files and not name.startswith('/') and '..' not in Path(name).parts and ':' not in name,'Unsafe or duplicate release member: '+name)
        plain(path)
        if expected:need(sha(path)==expected,'Release input mismatch: '+name)
        files[name]=path
    for name in spec['binaries']:
        expected=receipt['binaries'].get(name);need(expected,'Binary missing from build receipt: '+name);add(name,build/name,expected)
    for name,source in spec.get('assets',{}).items():
        expected=receipt.get('assets',{}).get(name);need(expected and expected==sha(plain(ROOT/source)),'Asset missing from build receipt: '+name);add(name,build/name,expected)
    need(receipt['binaries']['libmpv-2.dll']==mpv['runtime_sha256'],'Media runtime mismatch')
    pdfium=next(x for x in inventory['components'] if x['id']=='pdfium')
    if pdfium.get('observed_binary_sha256'):need(receipt['binaries']['pdfium.dll']==pdfium['observed_binary_sha256'],'PDFium runtime mismatch')
    for source,name in mpv['distribution_files'].items():
        if name=='libmpv-2.dll':continue
        add(name,build/name,mpv['files'][source]['sha256'])
    for name,notice in spec['upstream_assets'].items():add(name,build/name,sha(plain(ROOT/'licenses/third-party'/notice)))
    add('LICENSE',ROOT/'LICENSE',inventory['project_license_sha256'])
    for item in inventory['notice_files']:
        path=Path(item['path']);need(path.is_relative_to('licenses/third-party'),'Notice outside inventory root')
        add(path.as_posix(),ROOT/path,item['sha256'])
    # Explicit proof that package notices cover the runtime integrations.
    for component in inventory['components']:
        for notice in component.get('notices',[]):need(notice in files,'Missing component notice: '+notice)
    need(not output.exists(),'Use a fresh package output directory')
    output.parent.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='folderwing-package-',dir=output.parent) as tmp:
        stage=Path(tmp);app=stage/('Folderwing-'+version()+'-windows-x64');app.mkdir()
        for name,path in files.items():
            target=app/name;target.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(path,target)
            need(sha(target)==sha(path),'Package copy changed: '+name)
        notice='Folderwing application code: MIT. Third-party components retain their own terms.\n\n'
        notice+='See licenses/third-party and libmpv-licenses for complete preserved notices.\n'
        notice+='Media fallback: LGPL-3.0-or-later libmpv/FFmpeg; its exact corresponding-source archive accompanies this ZIP.\n'
        notice+='PDFium: controlled LLVM-MinGW build; PDFium/LLVM/MinGW/winpthreads retain their terms. Its exact recipe archive accompanies this ZIP.\n'
        notice+='Direct is distributed separately. Git, Everything and WSL are optional separately installed integrations.\n'
        if not inventory['release_ready']:notice+='This is a draft packaging candidate; dependency release-review items remain open in docs/DEPENDENCIES.md.\n'
        (app/'THIRD-PARTY-NOTICES.txt').write_text(notice)
        (app/'README.txt').write_text('Folderwing '+version()+' — Windows x64 portable build\n\nExtract this entire folder and open folderwing.exe. Keep all helper files together.\nSettings are saved in AppData\\Folderwing unless a portable folderwing.ini exists.\nLegacy OpenPilot settings migrate by copying; originals are retained.\nWSL, Git and Everything are optional. Direct is a separate companion.\nThis unsigned build may require confirmation by Windows when first downloaded.\nSee LICENSE and THIRD-PARTY-NOTICES.txt for licensing information.\n')
        buildinfo={'schema':'folderwing-package/v1','version':version(),'platform':'windows-x64','revision':receipt['revision'],'source_digest':receipt['source_digest'],'compiler_sha256':receipt['compiler_sha256'],'build_receipt_sha256':sha(build/'BUILD-RECEIPT.json'),'public_release_ready':inventory['release_ready'],'dependency_open_items':inventory['open_items'],'media_source_bundle':mpv['source_bundle'],'features':spec['minimum_feature_set'],'files':{p.relative_to(app).as_posix():{'sha256':sha(p),'bytes':p.stat().st_size} for p in sorted(app.rglob('*')) if p.is_file()}}
        (app/'BUILDINFO.json').write_text(json.dumps(buildinfo,indent=2)+'\n')
        archive=stage/(app.name+'.zip')
        with zipfile.ZipFile(archive,'x',compression=zipfile.ZIP_DEFLATED,compresslevel=6) as z:
            for file in sorted(app.rglob('*')):
                if not file.is_file():continue
                info=zipfile.ZipInfo(file.relative_to(stage).as_posix(),date_time=(2026,1,1,0,0,0));info.create_system=3
                info.external_attr=(0o100755 if file.name=='folderwing-wsl' else 0o100644)<<16
                info.compress_type=zipfile.ZIP_DEFLATED;z.writestr(info,file.read_bytes())
        shutil.rmtree(app)
        shutil.copyfile(source_archive,stage/source_archive.name)
        shutil.copyfile(recipe_archive,stage/recipe_archive.name)
        need(sha(stage/recipe_archive.name)==recipe['sha256'],'Packaged PDFium recipe checksum mismatch')
        need(sha(stage/source_archive.name)==mpv['source_bundle']['sha256'],'Packaged corresponding-source checksum mismatch')
        publication={'schema':'folderwing-release-artifacts/v1','version':version(),'public_release_ready':inventory['release_ready'],'revision':receipt['revision'],'source_digest':receipt['source_digest'],'artifacts':{p.name:{'sha256':sha(p),'bytes':p.stat().st_size} for p in sorted(stage.iterdir()) if p.is_file()},'binaries':receipt['binaries'],'build_receipt_sha256':sha(build/'BUILD-RECEIPT.json'),'direct_bundled':False,'user_settings_bundled':False}
        (stage/'RELEASE-MANIFEST.json').write_text(json.dumps(publication,indent=2)+'\n')
        (stage/'SHA256SUMS').write_text(''.join(sha(p)+'  '+p.name+'\n' for p in sorted(stage.iterdir()) if p.is_file()))
        need(not output.exists(),'Package output appeared during staging');stage.rename(output)
    return publication

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--build-directory',type=Path,required=True);p.add_argument('--media-source-archive',type=Path,required=True);p.add_argument('--output-directory',type=Path,required=True);p.add_argument('--publish-ready',action='store_true');a=p.parse_args()
    try:r=package(a.build_directory,a.media_source_archive,a.output_directory,a.publish_ready)
    except (ValueError,OSError,KeyError) as e:raise SystemExit(str(e))
    print(json.dumps(r,indent=2))
if __name__=='__main__':main()
