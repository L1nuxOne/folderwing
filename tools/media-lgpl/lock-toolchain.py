"""Run inside the owned builder: retain exact additional .debs for offline rebuild."""
from pathlib import Path
import subprocess,concurrent.futures,hashlib,json,re,shutil
root=Path('/work');target=root/'toolchain-debs';target.mkdir(exist_ok=True)
def parse(text):return {v[0]:(v[1],v[2]) for l in text.splitlines() if len(v:=l.split('\t'))==3}
base=parse((root/'base-packages.tsv').read_text());installed=parse(Path('/toolchain-record/packages.tsv').read_text());items=[(n,v,a) for n,(v,a) in installed.items() if base.get(n)!=(v,a)]
def fetch(item):
 n,v,a=item
 if not re.fullmatch(r'[a-zA-Z0-9+.:_-]+',n) or not re.fullmatch(r'[a-zA-Z0-9+.:~_-]+',v):raise ValueError('Invalid package name/version')
 out=subprocess.run(['apt-get','download',n+'='+v],cwd=target,capture_output=True,text=True)
 if out.returncode:raise RuntimeError(out.stderr)
 return {'name':n,'version':v,'architecture':a}
rows=list(concurrent.futures.ThreadPoolExecutor(4).map(fetch,items));files={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in target.glob('*.deb')};assert len(files)==len(rows)
record={'schema':'folderwing-lgpl-toolchain-lock/v1','base_image':'ubuntu@sha256:68ee22004a9109b44973313b02ff7b1de250f1de0f5ecb628d73f579aaff33ac','additional_packages':rows,'deb_files_sha256':files,'installed_packages_sha256':hashlib.sha256(Path('/toolchain-record/packages.tsv').read_bytes()).hexdigest()};(root/'toolchain-lock.json').write_text(json.dumps(record,indent=2)+'\n');shutil.copy('/toolchain-record/packages.tsv',root/'toolchain-packages.tsv')
notices=root/'toolchain-notices';notices.mkdir(exist_ok=True)
for name in installed:
 p=Path('/usr/share/doc')/name/'copyright'
 if p.exists():shutil.copyfile(p,notices/(name+'.copyright'))
print('Locked',len(files),'exact toolchain packages and',len(list(notices.iterdir())),'copyright records.')
