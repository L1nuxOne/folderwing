"""Focused WSL build for the action dock. Does not publish the release."""
import os
from pathlib import Path
from brand_resource import build_brand_resource
import re,subprocess,sys,json,hashlib,shutil,argparse
root=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser();p.add_argument('targets',nargs='*');p.add_argument('--output',default='build/action_dock/20261023/candidate');args=p.parse_args()
out=root/args.output;out.mkdir(parents=True,exist_ok=True);cohort=out.parent
w=lambda p:'C:/'+str(p).split('/mnt/c/',1)[1]
zig=(os.environ.get('ZIG') or str(root/'third_party/zig/zig.exe'))
common=[zig,'cc','-target','x86_64-windows-gnu','-std=c11','-municode','-O2','-DNDEBUG','-s','-DUNICODE','-D_UNICODE','-Wall','-Wextra','-Wno-unused-parameter','-Wno-missing-field-initializers','-Wno-unused-function','-I'+w(root/'include')]
text=(root/'build.ps1').read_text();appdef=text.split('$app = @(',1)[1].split("Invoke-Zig 'folderwing.exe'",1)[0];app=[root/x.replace('\\','/') for x in re.findall(r'"\$root\\([^"\r\n]+)"',appdef)]
engine=sorted((root/'src').glob('*.c'));libs=['-ladvapi32','-lkernel32','-luser32','-lshell32','-lole32','-lmpr','-lgdi32','-lmsimg32','-ldwmapi','-luxtheme','-luuid','-limm32']
recipes={}
recipes['folderwing.exe']=common+[w(p) for p in engine+app]+libs+['-Wl,--subsystem,windows']
recipes['actiondock_tests.exe']=common+[w(root/p) for p in ['tests/actiondock_tests.c','app/actiondock.c']]+['-lkernel32','-luser32','-lgdi32']
recipes['actiondock_host_tests.exe']=common+[w(p) for p in engine+app if p.name!='fpdemo.c']+[w(root/'tests/actiondock_host_tests.c')]+libs
targets=args.targets or ['folderwing.exe','actiondock_tests.exe','actiondock_host_tests.exe'];records=[]
for name in targets:
 if name not in recipes:raise SystemExit('Unknown target '+name)
 command=recipes[name]+([build_brand_resource(root,out,zig)] if name=='folderwing.exe' else [])+['-o',w(out/name)];print('Build',name,flush=True);r=subprocess.run(command,capture_output=True,text=True);(cohort/(name+'-build.log')).write_text(r.stdout+r.stderr)
 if r.returncode:print(r.stdout+r.stderr);raise SystemExit(r.returncode)
 if name=='folderwing-wsl':
  tmp=out/'folderwing-wsl.fresh';shutil.copyfile(out/name,tmp);tmp.chmod(0o755);tmp.replace(out/name)
 print(name,(out/name).stat().st_size,flush=True);records.append(dict(name=name,command=command,sha256=hashlib.sha256((out/name).read_bytes()).hexdigest()))
for source,target in [('libmpv-2.dll','libmpv-2.dll'),('COPYRIGHT.txt','libmpv-COPYRIGHT.txt'),('SOURCE.txt','libmpv-SOURCE.txt')]:shutil.copy2(root/'third_party/mpv'/source,out/target)
for name in ['folderwing-text.exe','folderwing-image.exe','folderwing-pdf.exe','pdfium.dll','folderwing-shell.exe','folderwing-operations.exe','folderwing-settings.exe','folderwing-zip.exe','folderwing-archive.exe','folderwing-durable.exe','folderwing-space.exe','folderwing-content.exe','folderwing-media.exe','folderwing-duplicates.exe','folderwing-terminal.exe','folderwing-diff.exe','folderwing-git.exe','direct-client.exe']:
 if (root/'build/release'/name).exists():shutil.copyfile(root/'build/release'/name,out/name)
if not (out/'folderwing-wsl').exists():
 tmp=out/'folderwing-wsl.owned-copy';shutil.copyfile(root/'build/release/folderwing-wsl',tmp);tmp.chmod(0o755);tmp.replace(out/'folderwing-wsl')
prior=json.loads((cohort/'BUILD.json').read_text()) if (cohort/'BUILD.json').exists() else []
by_name={item['name']:item for item in prior};by_name.update({item['name']:item for item in records});(cohort/'BUILD.json').write_text(json.dumps(list(by_name.values()),indent=2)+'\n')
