"""Acquire exact locked source archives. No branch/tag resolution or submodule fetch."""
from pathlib import Path
import argparse,hashlib,json,tarfile,urllib.request
p=argparse.ArgumentParser();p.add_argument('--work-directory',type=Path,required=True);p.add_argument('--lock',type=Path,default=Path(__file__).with_name('sources.json'));p.add_argument('--offline',action='store_true');a=p.parse_args()
lock=json.loads(a.lock.read_text());root=a.work_directory.resolve();root.mkdir(parents=True,exist_ok=True)
marker=root/'.folderwing-media-build'
if marker.exists() and marker.read_text()!='folderwing-lgpl-build/v1\n':raise SystemExit('Wrong work-directory owner marker')
marker.write_text('folderwing-lgpl-build/v1\n');(root/'archives').mkdir(exist_ok=True);(root/'src').mkdir(exist_ok=True)
rows=lock['sources']+json.loads(a.lock.with_name('headers.json').read_text())['sources']
for item in rows:
 name=item['name'];filename=item['archive']
 if Path(name).name!=name or Path(filename).name!=filename:raise SystemExit('Unsafe lock path')
 archive=root/'archives'/filename
 if not archive.exists():
  if a.offline:raise SystemExit('Missing offline archive: '+filename)
  if not item['url'].startswith('https://codeload.github.com/'):raise SystemExit('Unexpected source origin')
  temp=archive.with_suffix('.download')
  with urllib.request.urlopen(urllib.request.Request(item['url'],headers={'User-Agent':'Folderwing-LGPL-build'}),timeout=60) as response,temp.open('wb') as output:
   copied=0
   while block:=response.read(1024*1024):
    copied+=len(block)
    if copied>item['bytes']:raise SystemExit('Unexpected archive size')
    output.write(block)
  temp.replace(archive)
 if archive.stat().st_size!=item['bytes'] or hashlib.sha256(archive.read_bytes()).hexdigest()!=item['sha256']:raise SystemExit('Archive checksum mismatch: '+filename)
 dest=root/'src'/name
 if not dest.exists():
  with tarfile.open(archive) as tar:
   members=tar.getmembers();prefix=members[0].name.split('/')[0]
   for m in members:
    parts=Path(m.name).parts
    if not parts or parts[0]!=prefix or '..' in parts:raise SystemExit('Unsafe archive member')
    m.name=str(Path(*parts[1:])) if len(parts)>1 else '.'
    tar.extract(m,dest,filter='data')
 print(name,item['commit'],'verified')
(root/'sources.json').write_text(json.dumps(lock,indent=2)+'\n')

# Preserve the exact MinGW runtime source package alongside the media sources.
meta=json.loads(a.lock.with_name('toolchain-sources.json').read_text())
extra=[dict(archive='mingw-w64_'+meta['version']+'.dsc',url=meta['metadata_url'],sha256=meta['metadata_sha256'])]+meta['archives']
for item in extra:
 file=root/'archives'/item['archive']
 if not file.exists():
  if a.offline:raise SystemExit('Missing runtime source: '+item['archive'])
  with urllib.request.urlopen(item['url'],timeout=60) as response:file.write_bytes(response.read())
 if hashlib.sha256(file.read_bytes()).hexdigest()!=item['sha256']:raise SystemExit('Runtime source checksum mismatch')
