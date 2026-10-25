"""Verify release artifacts before draft publication; never uploads anything."""
import argparse,re
from pathlib import Path
from release_common import read,need,plain,sha
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--directory',type=Path,required=True);p.add_argument('--tag');p.add_argument('--revision');p.add_argument('--require-public-ready',action='store_true');a=p.parse_args()
r=read(plain(a.directory/'RELEASE-MANIFEST.json'))
need(re.fullmatch(r'\d+\.\d+\.\d+',r['version']),'Invalid release version')
if a.tag:need(a.tag=='v'+r['version'],'Tag must match the version compiled into Folderwing')
if a.revision:need(a.revision==r['revision'],'Release artifacts were not built from the tagged commit')
if a.require_public_ready:need(r['public_release_ready'] is True,'Dependency release-review items remain open; automatic publication refused')
expected=set(r['artifacts'])|{'RELEASE-MANIFEST.json','SHA256SUMS'}
need({x.name for x in a.directory.iterdir()}==expected,'Unexpected release artifact directory contents')
for name,record in r['artifacts'].items():
 need(Path(name).name==name,'Unsafe artifact name');path=plain(a.directory/name)
 need(path.stat().st_size==record['bytes'] and sha(path)==record['sha256'],'Artifact checksum/size mismatch: '+name)
lines=(a.directory/'SHA256SUMS').read_text().splitlines();sums={}
for line in lines:
 digest,name=line.split('  ',1);need(name not in sums and Path(name).name==name,'Unsafe/duplicate checksum entry');sums[name]=digest
need(set(sums)==expected-{'SHA256SUMS'},'Incomplete checksum file')
for name,digest in sums.items():need(sha(plain(a.directory/name))==digest,'Checksum list mismatch: '+name)
print('Verified Folderwing',r['version'],'artifacts; public_release_ready =',r['public_release_ready'])
