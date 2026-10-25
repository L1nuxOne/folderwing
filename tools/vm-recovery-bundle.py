#!/usr/bin/env python3
"""Package the private static Linux fixture and exact recovery-harness sources."""
import argparse
import gzip
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import struct
import tarfile
import sys

ROOT=Path(__file__).resolve().parents[1]
SOURCES=('linux/fpjobs.c','linux/fpwinprepare.h','linux/fpwingroup.h','linux/fpsha256.h','linux/fpmetadata.h','linux/fpstage.h','linux/fpmount.h','linux/fpancestry.h',
         'src/wslproto.h','tests/fixtures/vm_recovery.c','tools/vm-recovery-guest.py',
         'tools/vm-recovery.ps1','tools/vm-recovery-bundle.py','docs/VM_RECOVERY.md',
         'tests/vm-recovery-manifest.json')

def static_fixture(data):
    if len(data)<64 or len(data)>8*1024*1024:raise ValueError('invalid fixture size')
    header=struct.unpack('<16sHHIQQQIHHHHHH',data[:64])
    if data[:7]!=b'\x7fELF\x02\x01\x01' or header[1] not in (2,3) or header[2]!=62 or not header[4]:raise ValueError('expected a Linux x86-64 executable')
    offset,size,count=header[5],header[9],header[10]
    if size!=56 or not count or offset+size*count>len(data):raise ValueError('invalid ELF program headers')
    entry=False
    for i in range(count):
        kind,flags,where,address,_,file_size,memory_size,_=struct.unpack_from('<IIQQQQQQ',data,offset+i*size)
        if kind==3:raise ValueError('fixture has a dynamic interpreter; build with static musl')
        if kind==1:
            if file_size>memory_size or where+file_size>len(data):raise ValueError('invalid ELF load segment')
            if flags&1 and address<=header[4]<address+file_size:entry=True
    if not entry:raise ValueError('entry point is outside executable file-backed load segments')

def bundle(fixture,output):
    if fixture.is_symlink() or not fixture.is_file():raise ValueError('private fixture must be a regular file')
    data=fixture.read_bytes();static_fixture(data)
    spec=importlib.util.spec_from_file_location('opvm_guest',ROOT/'tools/vm-recovery-guest.py')
    guest=importlib.util.module_from_spec(spec);spec.loader.exec_module(guest)
    assets={'fixture':data,'guest.py':(ROOT/'tools/vm-recovery-guest.py').read_bytes()}
    assets.update({'source/'+n:(ROOT/n).read_bytes() for n in SOURCES})
    manifest={'schema':'openpilot-vm-bundle/v1','fixture_sha256':hashlib.sha256(data).hexdigest(),
              'phases':list(guest.PHASES),'payload':{'new_sha256':guest.sha_bytes(guest.NEW),'old_sha256':guest.sha_bytes(guest.OLD),'new_size':len(guest.NEW),'old_size':len(guest.OLD)},
              'files':{name:hashlib.sha256(value).hexdigest() for name,value in sorted(assets.items())},
              'scope':'Prepared eight-checkpoint native Linux file-move cohort; no guest interruption executed by packaging',
              'provenance':'Hashes bind the supplied static fixture and observed source snapshot; the owning build receipt records their compilation.'}
    assets['manifest.json']=(json.dumps(manifest,sort_keys=True,indent=2)+'\n').encode()
    with output.open('xb') as raw,gzip.GzipFile(filename='',mode='wb',fileobj=raw,mtime=0) as zipped,tarfile.open(fileobj=zipped,mode='w') as tar:
        for name,value in sorted(assets.items()):
            info=tarfile.TarInfo('openpilot-vm-recovery/'+name);info.size=len(value);info.mode=0o755 if name in ('fixture','guest.py') else 0o644;info.mtime=0
            tar.addfile(info,io.BytesIO(value))
    return manifest

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--fixture',type=Path,required=True);parser.add_argument('--output',type=Path,required=True);args=parser.parse_args()
    try:
        result=bundle(args.fixture,args.output)
        print(json.dumps({'outcome':'prepared','bundle_sha256':hashlib.sha256(args.output.read_bytes()).hexdigest(),'manifest':result},indent=2));return 0
    except (OSError,ValueError) as exc:
        print(json.dumps({'outcome':'error','error':str(exc)}),file=sys.stderr);return 2

if __name__=='__main__':sys.exit(main())
