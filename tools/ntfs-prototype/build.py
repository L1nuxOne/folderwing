"""Build the standalone read-only experiment; never updates the release."""
import os
from pathlib import Path
import argparse,subprocess,json,hashlib
root=Path(__file__).resolve().parents[2]
p=argparse.ArgumentParser();p.add_argument('--output',default='build/ntfs_mft_prototype/20261023');args=p.parse_args();out=root/args.output;out.mkdir(parents=True,exist_ok=True)
w=lambda p:'C:/'+str(p).split('/mnt/c/',1)[1]
inputs=['tools/ntfs-prototype/main.c','tools/ntfs-prototype/mft.c','tools/ntfs-prototype/mft.h','tools/ntfs-prototype/build.py']
command=[(os.environ.get('ZIG') or str(root/'third_party/zig/zig.exe')),'cc','-target','x86_64-windows-gnu','-std=c11','-municode','-O2','-DNDEBUG','-DUNICODE','-D_UNICODE','-Wall','-Wextra','-Wno-unused-parameter',w(root/inputs[0]),w(root/inputs[1]),'-lkernel32','-lpsapi','-o',w(out/'mftprobe.exe')]
r=subprocess.run(command,capture_output=True,text=True);(out/'build.log').write_text(r.stdout+r.stderr);print(r.stdout+r.stderr)
if r.returncode:raise SystemExit(r.returncode)
(out/'BUILD.json').write_text(json.dumps({'command':command,'inputs':{name:hashlib.sha256((root/name).read_bytes()).hexdigest() for name in inputs},'image_sha256':hashlib.sha256((out/'mftprobe.exe').read_bytes()).hexdigest()},indent=2)+'\n')
helper_command=[command[0],'cc','-target','x86_64-windows-gnu','-std=c11','-municode','-O2',w(root/'tools/ntfs-prototype/resident-allocation.c'),'-lkernel32','-o',w(out/'resident-allocation.exe')]
h=subprocess.run(helper_command,capture_output=True,text=True);(out/'resident-allocation-build.log').write_text(h.stdout+h.stderr)
if h.returncode:print(h.stderr);raise SystemExit(h.returncode)
(out/'RESIDENT_BUILD.json').write_text(json.dumps({'command':helper_command,'source_sha256':hashlib.sha256((root/'tools/ntfs-prototype/resident-allocation.c').read_bytes()).hexdigest(),'image_sha256':hashlib.sha256((out/'resident-allocation.exe').read_bytes()).hexdigest()},indent=2)+'\n')
print('Built',out/'mftprobe.exe')
