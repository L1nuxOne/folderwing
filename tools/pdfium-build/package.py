"""Create a private PDFium candidate ZIP with prerequisites and review evidence.

No public-release mode: /MD still incorporates Microsoft CRT startup objects.
"""
from __future__ import annotations
import argparse,hashlib,json,shutil,tempfile,zipfile
from pathlib import Path
from audit import audit,need
HERE=Path(__file__).resolve().parent;ROOT=HERE.parents[1]
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--build-directory',type=Path,required=True);p.add_argument('--source-root',type=Path,required=True);p.add_argument('--baseline-dll',type=Path,required=True);p.add_argument('--output-directory',type=Path,required=True);a=p.parse_args();b=a.build_directory;out=a.output_directory
 need(not out.exists(),'Use a new candidate output directory');r=audit(b/'pdfium.dll',b/'pdfium.map',a.baseline_dll,b/'commands.txt');pin=json.loads((HERE/'inputs.json').read_text());need(r['external_runtime_imports_verified'],'External runtime not verified')
 build=json.loads((b/'BUILD.json').read_text());need(build['outcome']=='built-candidate','Missing controlled build receipt');need(build['inputs_sha256']==sha(HERE/'inputs.json'),'Input pins changed since build');need(build['recipe_sha256']==sha(HERE/'build.py'),'Recipe changed since build')
 for n,h in build['files'].items():need(sha(b/n)==h,'Build artifact changed: '+n)
 for row in json.loads((HERE/'notice-sources.json').read_text()):need(sha(HERE/row['path'])==row['sha256'],'Candidate notice changed: '+row['path'])
 out.parent.mkdir(parents=True,exist_ok=True)
 with tempfile.TemporaryDirectory(prefix='pdfium-candidate-',dir=out.parent) as temp:
  stage=Path(temp);app=stage/'folderwing-pdfium-external-crt-candidate-20261025';app.mkdir()
  for src,name in [(b/'pdfium.dll','bin/pdfium.dll'),(b/'pdfium.dll.lib','lib/pdfium.dll.lib'),(b/'args.gn','args.gn')]:
   target=app/name;target.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(src,target)
  # The Folderwing helper uses the C interface. Do not ship the upstream C++
  # convenience wrappers with their source-tree-relative include paths.
  include=app/'include';include.mkdir()
  need({f.name for f in (a.source_root/'public').glob('*.h')}==set(build['public_headers_sha256']),'Public C header set changed since build')
  for name,h in build['public_headers_sha256'].items():
   header=a.source_root/'public'/name;need(sha(header)==h,'Public C header changed since build: '+name);shutil.copyfile(header,include/name)
  shutil.copytree(ROOT/'licenses/third-party/pdfium',app/'licenses/pdfium');shutil.copytree(HERE/'licenses',app/'licenses/build-tool-runtime-notices')
  (app/'licenses/pdfium/MICROSOFT-RUNTIME-NOTE.txt').write_text('Candidate built with /MD. UCRT and VCRUNTIME140/140_1 are external prerequisites.\nMicrosoft CRT startup/support object code is still incorporated; inspect BUILDINFO.json.\nPublic redistribution has not been cleared. Folderwing MIT does not relicense these portions.\n')
  (app/'VERSION').write_text('MAJOR=157\nMINOR=0\nBUILD=8086\nPATCH=0\n')
  (app/'PREREQUISITES.md').write_text((HERE/'PREREQUISITES.md').read_text())
  (app/'THIRD-PARTY-NOTICES.txt').write_text('PDFium candidate: BSD-3-Clause and component notices under licenses/.\nThis candidate also contains Microsoft startup/support code, separately licensed.\nThe Microsoft runtime DLLs are external prerequisites and are not in this archive.\nThis package is a tested private candidate; it is not approved for public distribution.\n')
  info={'schema':'folderwing-pdfium-candidate/v1','version':pin['version'],'source_commit':pin['source_commit'],'source_graph_sha256':pin['source_graph_sha256'],'toolchain_lock_sha256':pin['toolchain_lock_sha256'],'inputs_sha256':sha(HERE/'inputs.json'),'build_receipt_sha256':sha(b/'BUILD.json'),'audit':r,'interface':'C API headers only','prerequisite':'Official Microsoft Visual C++ v14 x64 runtime, 14.44.35229 or newer; Windows 10+ UCRT','public_release_ready':False,'files':{f.relative_to(app).as_posix():sha(f) for f in sorted(app.rglob('*')) if f.is_file()}}
  need([f.name for f in app.rglob('*.dll')]==['pdfium.dll'],'Unexpected bundled runtime DLL')
  (app/'BUILDINFO.json').write_text(json.dumps(info,indent=2)+'\n')
  archive=stage/(app.name+'.zip')
  with zipfile.ZipFile(archive,'x',zipfile.ZIP_DEFLATED,compresslevel=6) as z:
   for f in sorted(app.rglob('*')):
    if not f.is_file():continue
    zi=zipfile.ZipInfo(f.relative_to(stage).as_posix(),date_time=(2026,1,1,0,0,0));zi.create_system=3;zi.external_attr=0o100644<<16;zi.compress_type=zipfile.ZIP_DEFLATED;z.writestr(zi,f.read_bytes())
  shutil.rmtree(app)
  manifest={'schema':'folderwing-pdfium-candidate-artifacts/v1','public_release_ready':False,'artifacts':{archive.name:{'sha256':sha(archive),'bytes':archive.stat().st_size}},'runtime_sha256':r['dll_sha256'],'external_runtime_dlls_bundled':False}
  (stage/'CANDIDATE-MANIFEST.json').write_text(json.dumps(manifest,indent=2)+'\n');(stage/'SHA256SUMS').write_text(''.join(sha(f)+'  '+f.name+'\n' for f in sorted(stage.iterdir()) if f.is_file()))
  need(not out.exists(),'Output appeared during staging');stage.rename(out)
 print(json.dumps(manifest,indent=2))
if __name__=='__main__':
 try:main()
 except (ValueError,OSError,KeyError) as e:raise SystemExit(str(e))
