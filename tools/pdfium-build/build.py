"""Build the pinned Windows x64 PDFium candidate; never install or publish it.

Source and toolchain restoration are described in README.md. Existing source
patches must match the recipe exactly; no reset/clean of arbitrary trees occurs.
"""
from __future__ import annotations
import argparse, hashlib, json, os, re, shutil, subprocess, sys, time
from pathlib import Path
HERE=Path(__file__).resolve().parent

def sha(p):
 h=hashlib.sha256()
 with p.open('rb') as f:
  for b in iter(lambda:f.read(1024*1024),b''):h.update(b)
 return h.hexdigest()
def read(p):return json.loads(p.read_text(encoding='utf-8-sig'))
def need(ok,msg):
 if not ok:raise ValueError(msg)
def run(args,**kw):return subprocess.run([str(x) for x in args],check=True,**kw)
def git_output(git,folder,*args):return subprocess.check_output([str(git),'-c','core.safecrlf=false','-C',str(folder),*args],text=True).strip()
def file_pin(root,name,expected):
 p=root/name;need(p.is_file() and not p.is_symlink(),'Missing/linked input: '+str(p));need(sha(p)==expected,'Pinned input changed: '+str(p));return p

def main():
 ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--source-root',type=Path,required=True);ap.add_argument('--toolchain-root',type=Path,required=True);ap.add_argument('--git',type=Path,required=True);ap.add_argument('--python',type=Path,required=True);ap.add_argument('--jobs',type=int,default=3);ap.add_argument('--output-directory',type=Path,required=True);a=ap.parse_args()
 need(os.name=='nt','Use the Windows Python interpreter');need(1<=a.jobs<=8,'Use between 1 and 8 workers')
 src=a.source_root.resolve();tc=a.toolchain_root.resolve();dest=a.output_directory.resolve();pin=read(HERE/'inputs.json')
 need(not dest.exists(),'Use a new receipt/output directory');need((src.parent/'.folderwing-owned-pdfium-build').is_file(),'Source workspace must have the owned-build marker')
 file_pin(a.python.parent,a.python.name,pin['build_python_executable_sha256'])
 file_pin(tc,'setup_x64.bat',pin['portable_setup_sha256'])
 need(git_output(a.git,src,'rev-parse','HEAD')==pin['source_commit'],'PDFium revision mismatch')
 graph=read(file_pin(HERE,'source-graph.json',pin['source_graph_sha256']))
 file_pin(HERE,'toolchain.lock.json',pin['toolchain_lock_sha256'])
 for name,v in graph.items():
  if v.get('rev') and re.fullmatch('[0-9a-f]{40}',v['rev']):
   dep=src.parent/name;need(git_output(a.git,dep,'rev-parse','HEAD')==v['rev'],'Source dependency mismatch: '+name)
   if dep not in (src,src/'build'):need(not git_output(a.git,dep,'diff','--name-only'),'Unexpected dependency source edits: '+name)
 tools={n:file_pin(src,n,h) for n,h in pin['tools'].items()}
 for v in pin['microsoft_tools']:file_pin(tc,v['path'],v['sha256'])
 # Apply exact build/interface patches only. git apply --check refuses a patch
 # on unrelated modifications; reverse --check permits an identical prior patch.
 for name,folder in [('pdfium.patch',src),('chromium-build.patch',src/'build')]:
  patch=file_pin(HERE,'patches/'+name,pin['patches'][name])
  reverse=subprocess.run([str(a.git),'-C',str(folder),'apply','--reverse','--check',str(patch)],capture_output=True)
  if reverse.returncode:
   run([a.git,'-C',folder,'apply','--check',patch]);run([a.git,'-C',folder,'apply',patch])
 # Reject extra dirty source changes, including edits elsewhere in a patched file.
 for name,folder in [('pdfium.patch',src),('chromium-build.patch',src/'build')]:
  actual=subprocess.check_output([str(a.git),'-c','core.safecrlf=false','-C',str(folder),'diff','--no-ext-diff','--no-color','--full-index','--binary','--'])
  need(actual.replace(b'\r\n',b'\n')==(HERE/'patches'/name).read_bytes().replace(b'\r\n',b'\n'),'Source changes exceed the controlled patch: '+str(folder))
 # The portable toolchain needs only an x64 vcvars shim; it changes no registry
 # or global PATH. Validate both architecture and SDK arguments.
 bat='@echo off\nif /I not "%~1"=="amd64" exit /b 64\nif /I not "%~2"=="10.0.26100.0" exit /b 65\ncall "%~dp0..\\setup_x64.bat"\n'
 shim=tc/'VC/vcvarsall.bat'
 if shim.exists():need(shim.read_text()==bat,'Existing vcvars shim differs')
 else:shim.write_text(bat,newline='\r\n')
 env=os.environ.copy();env.pop('PYTHONHOME',None);env['DEPOT_TOOLS_WIN_TOOLCHAIN']='0';env['DEPOT_TOOLS_UPDATE']='0';env['VPYTHON_BYPASS']='manually managed python not supported by chrome operations';env['GYP_MSVS_OVERRIDE_PATH']=str(tc);env['VCToolsVersion']=pin['msvc_version']
 env['PATH']=str(a.python.parent)+';'+str(a.git.parent)+';'+str(tc/'Windows Kits/10/bin'/pin['sdk_version']/'x64')+';'+env['PATH']
 out=src/'out/folderwing';out.mkdir(parents=True,exist_ok=True)
 flags={k:False for k in ['is_component_build','is_debug','pdf_enable_v8','pdf_enable_xfa','pdf_use_partition_alloc','pdf_use_skia','pdf_enable_brotli','pdf_enable_fontations','pdf_enable_rust_bmp','pdf_enable_rust_jpeg','pdf_enable_rust_png','treat_warnings_as_errors','use_remoteexec','use_siso']}
 flags.update(pdf_is_standalone=True,target_cpu='x64',target_os='win',symbol_level=1,visual_studio_path=tc.as_posix(),visual_studio_version='2022',windows_sdk_path=(tc/'Windows Kits/10').as_posix(),windows_sdk_version=pin['sdk_version'],wdk_path=(tc/'Windows Kits/10').as_posix())
 args=''.join(k+' = '+json.dumps(v)+'\n' for k,v in flags.items());(out/'args.gn').write_text(args,newline='\n')
 dest.mkdir(parents=True);started=time.monotonic()
 source_diff={}
 for folder in [src,src/'build']:source_diff[str(folder)]=git_output(a.git,folder,'diff','--')
 before={'schema':'folderwing-controlled-pdfium-build/v1','recipe_sha256':sha(Path(__file__)),'inputs_sha256':sha(HERE/'inputs.json'),'source_graph_sha256':pin['source_graph_sha256'],'patches':pin['patches'],'args_gn':args,'source_diff_sha256':{n:hashlib.sha256(s.encode()).hexdigest() for n,s in source_diff.items()},'public_headers_sha256':{p.name:sha(p) for p in sorted((src/'public').glob('*.h'))},'jobs':a.jobs,'purpose':'candidate build; no public redistribution claim','wsl_stopped':False}
 (dest/'INPUT.json').write_text(json.dumps(before,indent=2)+'\n')
 run([a.python,src/'build/util/lastchange.py','-o',src/'build/util/LASTCHANGE'],cwd=src,env=env)
 for label,cmd in [('gn',[tools['buildtools/win/gn.exe'],'gen','out/folderwing']),('ninja',[tools['third_party/ninja/ninja.exe'],'-C','out/folderwing','-j'+str(a.jobs),'-d','keeprsp','pdfium'])]:
  with (dest/(label+'.log')).open('wb') as f:run(cmd,cwd=src,env=env,stdout=f,stderr=subprocess.STDOUT)
 for folder,old in source_diff.items():need(git_output(a.git,folder,'diff','--')==old,'Source changed during build: '+folder)
 for n,h in before['public_headers_sha256'].items():need(sha(src/'public'/n)==h,'Public header changed during build: '+n)
 # Keep link provenance and full commands; final audit must inspect Microsoft
 # startup/import archive members instead of equating /MD with no static code.
 with (dest/'commands.txt').open('wb') as f:run([tools['third_party/ninja/ninja.exe'],'-C',out,'-t','commands','pdfium'],env=env,stdout=f)
 commands=(dest/'commands.txt').read_text(encoding='utf-8-sig');need(not re.search(r'(?i)(?:^|\s)/MT(?:d)?(?:\s|$)',commands),'Static CRT compilation escaped policy')
 for n in ['pdfium.dll','pdfium.dll.lib','pdfium.map','pdfium-lld.map','args.gn']:shutil.copyfile(out/n,dest/n)
 result={**before,'outcome':'built-candidate','elapsed_seconds':round(time.monotonic()-started,3),'files':{p.name:sha(p) for p in dest.iterdir() if p.is_file()},'public_release_ready':False,'runtime_link_audit_required':True}
 (dest/'BUILD.json').write_text(json.dumps(result,indent=2)+'\n');print('PDFium candidate:',dest)

if __name__=='__main__':
 try:main()
 except (ValueError,OSError,subprocess.CalledProcessError) as e:raise SystemExit(str(e))
