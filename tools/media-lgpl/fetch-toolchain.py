"""Download the locked build-tool .debs. They are build inputs, not app assets."""
from pathlib import Path
import argparse,hashlib,json,urllib.request,concurrent.futures
p=argparse.ArgumentParser();p.add_argument('--directory',type=Path,required=True);p.add_argument('--lock',type=Path,default=Path(__file__).with_name('toolchain.json'));a=p.parse_args();lock=json.loads(a.lock.read_text());a.directory.mkdir(parents=True,exist_ok=True)
def fetch(row):
 n=row['filename']
 if Path(n).name!=n:raise ValueError('Unsafe filename')
 file=a.directory/n
 if not file.exists():
  temp=file.with_suffix('.download')
  with urllib.request.urlopen(row['url'],timeout=60) as r,temp.open('wb') as out:
   count=0
   while block:=r.read(1024*1024):
    count+=len(block)
    if count>row['bytes']:raise ValueError('Unexpected size')
    out.write(block)
  temp.replace(file)
 if file.stat().st_size!=row['bytes'] or hashlib.sha256(file.read_bytes()).hexdigest()!=row['sha256']:raise ValueError('Toolchain checksum mismatch: '+n)
 return n
for n in concurrent.futures.ThreadPoolExecutor(4).map(fetch,lock['deb_downloads']):print(n,'verified')
