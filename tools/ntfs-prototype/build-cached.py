"""Build only the standalone cache/USN experiment, not the application."""
import os
from pathlib import Path
import argparse, hashlib, json, subprocess
root=Path(__file__).resolve().parents[2]
p=argparse.ArgumentParser();p.add_argument('--output',default='build/ntfs_cached_model/20261023/candidate');a=p.parse_args()
out=root/a.output;out.mkdir(parents=True,exist_ok=True)
def win(path):return 'C:/'+str(path).split('/mnt/c/',1)[1]
inputs=['cached.c','main.c','mft.c','mft.h','build-cached.py']
command=[(os.environ.get('ZIG') or str(root/'third_party/zig/zig.exe')),'cc','-target','x86_64-windows-gnu','-std=c11','-municode','-O2','-DUNICODE','-D_UNICODE','-Wall','-Wextra','-Wno-unused-parameter',win(root/'tools/ntfs-prototype/cached.c'),win(root/'tools/ntfs-prototype/mft.c'),'-lkernel32','-lpsapi','-lbcrypt','-o',win(out/'cachedprobe.exe')]
r=subprocess.run(command,capture_output=True,text=True);(out/'build.log').write_text(r.stdout+r.stderr);print(r.stdout+r.stderr)
if r.returncode:raise SystemExit(r.returncode)
(out/'BUILD.json').write_text(json.dumps({'command':command,'inputs':{x:hashlib.sha256((root/'tools/ntfs-prototype'/x).read_bytes()).hexdigest() for x in inputs},'image_sha256':hashlib.sha256((out/'cachedprobe.exe').read_bytes()).hexdigest()},indent=2)+'\n');print(out/'cachedprobe.exe')
