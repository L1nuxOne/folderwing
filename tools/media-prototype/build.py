import os
from pathlib import Path
import subprocess,hashlib,json,shutil
root=Path(__file__).resolve().parents[2];cohort=root/'build/media_prototype/20261023';out=cohort/'candidate';out.mkdir(exist_ok=True)
w=lambda p:'C:/'+str(p).split('/mnt/c/',1)[1]
zig=(os.environ.get('ZIG') or str(root/'third_party/zig/zig.exe'))
command=[zig,'cc','-target','x86_64-windows-gnu','-std=c++17','-fno-exceptions','-fno-rtti','-fms-extensions','-municode','-O2','-s','-DNDEBUG','-DUNICODE','-D_UNICODE','-Wall','-Wextra','-Wno-unused-parameter','-Wno-missing-field-initializers','-I'+w(cohort/'dependencies/libmpv/include'),w(root/'tools/media-prototype/player.cpp'),'-lkernel32','-luser32','-lgdi32','-lole32','-loleaut32','-luuid','-ld3d11','-ldxgi','-ldxguid','-lmfplat','-lmfuuid','-lmf','-lpropsys','-lpsapi','-lpdh','-o',w(out/'media-prototype.exe')]
r=subprocess.run(command,capture_output=True,text=True);(cohort/'build.log').write_text(r.stdout+r.stderr);print(r.stdout+r.stderr);assert r.returncode==0
shutil.copyfile(cohort/'dependencies/libmpv/libmpv-2.dll',out/'libmpv-2.dll')
(cohort/'BUILD.json').write_text(json.dumps({'command':command,'source_sha256':hashlib.sha256((root/'tools/media-prototype/player.cpp').read_bytes()).hexdigest(),'images':{p.name:{'bytes':p.stat().st_size,'sha256':hashlib.sha256(p.read_bytes()).hexdigest()} for p in [out/'media-prototype.exe',out/'libmpv-2.dll']}},indent=2));print('Built media-prototype.exe', (out/'media-prototype.exe').stat().st_size)

probe_command=[zig,'cc','-target','x86_64-windows-gnu','-fno-exceptions','-fno-rtti','-O2','-s',w(root/'tools/media-prototype/audio-probe.cpp'),'-lole32','-luuid','-o',w(out/'audio-probe.exe')]
r=subprocess.run(probe_command,capture_output=True,text=True);(cohort/'audio-probe-build.log').write_text(r.stdout+r.stderr);assert r.returncode==0,r.stdout+r.stderr
(cohort/'AUDIO_PROBE_BUILD.json').write_text(json.dumps({'command':probe_command,'source_sha256':hashlib.sha256((root/'tools/media-prototype/audio-probe.cpp').read_bytes()).hexdigest(),'sha256':hashlib.sha256((out/'audio-probe.exe').read_bytes()).hexdigest()},indent=2))
