"""Seal the local DLL and its corresponding source/notices bundle; never publishes."""
from pathlib import Path
import argparse,json,hashlib,re,shutil,subprocess,tarfile,zipfile
p=argparse.ArgumentParser();p.add_argument('--build-id',default='20261025');p.add_argument('--work-directory',type=Path,required=True);p.add_argument('--output-directory',type=Path,required=True);a=p.parse_args();w=a.work_directory.resolve();out=a.output_directory.resolve();out.mkdir(parents=True,exist_ok=True);recipe=Path(__file__).resolve().parent;sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
assert re.fullmatch(r'[0-9A-Za-z][0-9A-Za-z.-]{0,79}',a.build_id),'Unsafe build identifier'
lock=json.loads((recipe/'sources.json').read_text());headers=json.loads((recipe/'headers.json').read_text());rows=lock['sources']+headers['sources'];cfg=(w/'build/ffmpeg/config.h').read_text()
for name,value in [('CONFIG_GPL','0'),('CONFIG_NONFREE','0'),('CONFIG_VERSION3','1'),('CONFIG_NETWORK','0'),('CONFIG_LIBX264','0'),('CONFIG_LIBX265','0')]:
 assert re.search(r'^#define '+name+r' '+value+r'$',cfg,re.M),(name,value)
opts={v['name']:v['value'] for v in json.loads((w/'build/mpv/meson-info/intro-buildoptions.json').read_text())}
assert opts['gpl'] is False and opts['d3d-hwaccel']=='enabled' and opts['d3d9-hwaccel']=='enabled' and opts['d3d11']=='enabled' and opts['lua']=='disabled' and opts['javascript']=='disabled'
for row in rows:assert sha(w/'archives'/row['archive'])==row['sha256']
dll=w/'output/libmpv-2.dll';pe=subprocess.check_output(['objdump','-p',str(dll)],text=True);imports=sorted(set(re.findall(r'DLL Name: (\S+)',pe)))
for name in imports:assert not re.match(r'(?i)(lib(gcc|stdc|winpthread|av|mpv|placebo)|shaderc|spirv)',name),name
version='folderwing-lgpl-'+a.build_id;stage=out/'runtime-package'
if stage.exists():raise SystemExit('Use a fresh output directory')
stage.mkdir();shutil.copy2(dll,stage/dll.name);shutil.copytree(w/'output/include',stage/'include')
notice_index=[]
for row in rows:
 source=w/'src'/row['name'];dest=stage/'LICENSES'/row['name'];dest.mkdir(parents=True,exist_ok=True)
 for file in source.rglob('*'):
  if file.is_symlink() or not file.is_file():continue
  rel=file.relative_to(source)
  # Submodules have their own locked entries. Avoid following or duplicating them.
  if any(part in ['.git','third_party','3rdparty'] for part in rel.parts):continue
  name=file.name.upper()
  if name.startswith(('LICENSE','COPYING','COPYRIGHT','NOTICE','AUTHORS','PATENTS')) or name in ['FTL.TXT','GPLV2.TXT'] or any(part.upper() in ['LICENSE','LICENSES'] for part in rel.parts[:-1]):
   target=dest/rel;target.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(file,target);notice_index.append(str(target.relative_to(stage)))
# System CRT, pthread and GCC runtime exception notices accompany statically linked routines.
for file in (w/'toolchain-notices').glob('*'):
 if any(v in file.name for v in ['mingw','libstdc++','libgcc']):
  dest=stage/'LICENSES/toolchain'/file.name;dest.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(file,dest);notice_index.append(str(dest.relative_to(stage)))
assert (stage/'LICENSES/freetype/docs/FTL.TXT').exists()
api_notice='mpv public API header notices (ISC); these notices do not license the libmpv DLL.\n\n'
for header in sorted((w/'src/mpv/include/mpv').glob('*.h')):
 api_notice+=header.name+'\n'+header.read_text().split('*/',1)[0]+'*/\n\n'
(stage/'API-NOTICES.txt').write_text(api_notice)
(stage/'COPYRIGHT.txt').write_text((w/'src/mpv/Copyright').read_text())
(stage/'THIRD-PARTY-NOTICES.txt').write_text('Folderwing media fallback: libmpv and FFmpeg, distributed under LGPL-3.0-or-later with their component notices. Folderwing application code remains MIT.\n\nThis software is based in part on the work of the FreeType Team; the FreeType License is selected for this build.\n\nThe DLL includes LGPL mpv, FFmpeg, libplacebo and FriBidi, permissive subtitle/font/codec/shader dependencies, and compiler runtimes under their respective licenses/exceptions. GPL-only media features and nonfree components are disabled.\n\nSee LICENSES for preserved component terms. See SOURCE.txt for exact corresponding sources and rebuild instructions. The DLL may be replaced with a compatible modified build; no application runtime hash/signature restriction is imposed.\n')
metadata={'schema':'folderwing-lgpl-media-build/v1','version':version,'combined_license':'LGPL-3.0-or-later','dll_sha256':sha(dll),'dll_bytes':dll.stat().st_size,'base_image':lock['base_image'],'sources':rows,'toolchain_lock_sha256':sha(recipe/'toolchain.json'),'ffmpeg_license_configuration':{'gpl':False,'nonfree':False,'version3':True,'network':False,'encoding':False},'mpv_options':{n:opts[n] for n in ['gpl','libmpv','cplayer','d3d11','d3d-hwaccel','d3d9-hwaccel','shaderc','spirv-cross','lua','javascript','wasapi']},'pe_imports':imports,'notice_files':sorted(notice_index)}
# No sources are fetched by the compiler build. Archive all exact originals and recipes.
srcdir=out/'source-package';srcdir.mkdir();(srcdir/'archives').mkdir();(srcdir/'recipe').mkdir();(srcdir/'build-records').mkdir()
runtime_sources=json.loads((recipe/'toolchain-sources.json').read_text())
archive_rows=rows+runtime_sources['archives']+[{'archive':'mingw-w64_'+runtime_sources['version']+'.dsc','sha256':runtime_sources['metadata_sha256']}]
for row in archive_rows:
 file=w/'archives'/row['archive'];assert sha(file)==row['sha256'];shutil.copyfile(file,srcdir/'archives'/file.name)
for file in recipe.iterdir():
 if file.is_file() and file.suffix not in ['.pyc']:shutil.copyfile(file,srcdir/'recipe'/file.name)
shutil.copyfile(w/'toolchain-packages.tsv',srcdir/'build-records/toolchain-packages.tsv')
for name,path in [('ffmpeg-config.h','build/ffmpeg/config.h'),('ffmpeg-config.mak','build/ffmpeg/ffbuild/config.mak'),('mpv-buildoptions.json','build/mpv/meson-info/intro-buildoptions.json')]:shutil.copyfile(w/path,srcdir/'build-records'/name)
for file in (w/'logs').glob('*.command'):shutil.copyfile(file,srcdir/'build-records'/file.name)
(srcdir/'BUILDINFO.json').write_text(json.dumps(metadata,indent=2)+'\n');shutil.copytree(stage/'LICENSES',srcdir/'LICENSES')
(srcdir/'REBUILD.md').write_text('''# Rebuild the Folderwing LGPL media DLL\n\nAll library sources are the original checksum-pinned archives in archives/.\nRun from the extracted source-package directory on Linux/WSL with Docker:\n\n```sh\npython3 recipe/acquire.py --lock recipe/sources.json --work-directory "$PWD" --offline\nmkdir -p toolchain-context\npython3 recipe/fetch-toolchain.py --lock recipe/toolchain.json --directory toolchain-context/toolchain-debs\ncp recipe/Dockerfile.locked toolchain-context/Dockerfile\ndocker build -t folderwing-media-locked toolchain-context\ndocker run --rm --network none --cpus 4 --memory 7g --memory-swap 9g --mount "type=bind,source=$PWD,target=/work" folderwing-media-locked bash /work/recipe/build.sh\n```\n\nThe exact additional toolchain package URLs, versions and SHA256 digests are\nlocked; the Ubuntu base image is pinned by digest. Toolchain .debs are build\ninputs downloaded from Ubuntu, not app assets. The media compile runs without\nnetwork access. Original sources, MinGW runtime source/packaging patches,\ncompiler-package notices and configure commands are included. No version is\nresolved from a moving branch. Output: output/libmpv-2.dll.\n\nInput reproducibility is provided; byte-for-byte rebuild identity on a second\nmachine has not been claimed. See BUILDINFO.json and build-records/.\n''')
source_bundle=out/('folderwing-media-lgpl-sources-'+a.build_id+'.tar.gz')
with tarfile.open(source_bundle,'w:gz',compresslevel=5) as tar:tar.add(srcdir,arcname='folderwing-media-lgpl-sources-'+a.build_id)
metadata['source_bundle']={'filename':source_bundle.name,'sha256':sha(source_bundle),'bytes':source_bundle.stat().st_size}
(stage/'BUILDINFO.json').write_text(json.dumps(metadata,indent=2)+'\n')
(stage/'SOURCE.txt').write_text('Folderwing minimal LGPL media build '+version+'\nCorresponding sources: '+source_bundle.name+'\nSHA256: '+sha(source_bundle)+'\nThe source archive contains exact original sources, build recipes, toolchain locks, patches/notices and REBUILD.md. Keep this archive available beside every download of this DLL.\nmpv: '+next(v['commit'] for v in rows if v['name']=='mpv')+'\nFFmpeg: '+next(v['commit'] for v in rows if v['name']=='ffmpeg')+'\n')
archive=out/('folderwing-media-lgpl-'+a.build_id+'.zip')
with zipfile.ZipFile(archive,'w',compression=zipfile.ZIP_DEFLATED,compresslevel=7) as z:
 for f in sorted(stage.rglob('*')):
  if f.is_file():z.write(f,str(f.relative_to(stage)))
files={str(f.relative_to(stage)):{'sha256':sha(f),'bytes':f.stat().st_size} for f in sorted(stage.rglob('*')) if f.is_file()}
distribution={n:('libmpv-licenses/'+n[len('LICENSES/'):] if n.startswith('LICENSES/') else {'API-NOTICES.txt':'libmpv-API-NOTICES.txt','COPYRIGHT.txt':'libmpv-COPYRIGHT.txt','SOURCE.txt':'libmpv-SOURCE.txt','BUILDINFO.json':'libmpv-BUILDINFO.json','THIRD-PARTY-NOTICES.txt':'libmpv-THIRD-PARTY-NOTICES.txt'}.get(n,n)) for n in files if not n.startswith('include/')}
manifest={'schema':2,'version':version,'license':'LGPL-3.0-or-later','archive_name':archive.name,'archive_url':None,'archive_sha256':sha(archive),'runtime_sha256':sha(dll),'source_bundle':metadata['source_bundle'],'recipe':'tools/media-lgpl','files':files,'distribution_files':distribution}
(out/'media-dependencies.json').write_text(json.dumps(manifest,indent=2)+'\n');print(json.dumps({'dll_bytes':dll.stat().st_size,'dll_sha256':sha(dll),'runtime_zip':str(archive),'source_bundle':metadata['source_bundle'],'notices':len(notice_index)},indent=2))
