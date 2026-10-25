#!/usr/bin/env python3
"""Prepare/observe an isolated Linux VM recovery case; never format or mount disks."""
from __future__ import annotations
import argparse
import contextlib
import hashlib
import json
import os
from pathlib import Path
import select
import signal
import stat
import struct
import subprocess
import sys
import time
import uuid

PHASES = ('partial-write','before-payload-sync','after-payload-sync',
          'before-destination-sync','after-publication-sync',
          'before-source-unlink','after-source-unlink','after-source-sync')
NEW = (b'OpenPilotVM-new\0' + bytes(range(256))) * 257
OLD = b'OpenPilotVM-old\0previous durable destination\n' * 113
MARKER = '.openpilot-recovery-volume.json'
PREFIX = b'OPVM1 '
RUN_ROOT = Path('/run')
HEAD = struct.Struct('<III')
PROGRESS = struct.Struct('<QQIIIIIiII')

class RecoveryError(ValueError):
    pass

def require(ok, text):
    if not ok: raise RecoveryError(text)

def token(value):
    require(isinstance(value,str) and uuid.UUID(value).hex == value, 'expected a canonical 32-character UUID')
    return value

def unique(pairs):
    result={}
    for key,value in pairs:
        require(key not in result,'duplicate JSON key: '+key);result[key]=value
    return result

def read_json(path):
    require(path.is_file() and not path.is_symlink() and path.stat().st_size<=32768,'invalid or oversized manifest')
    return json.loads(path.read_text(),object_pairs_hook=unique)

def sha_bytes(data):return hashlib.sha256(data).hexdigest()
def boot_id():return Path('/proc/sys/kernel/random/boot_id').read_text().strip()

def require_guest():
    release=Path('/proc/sys/kernel/osrelease').read_text().lower()
    require('wsl' not in release,'Refusing to arm/provision inside WSL; use the disposable Hyper-V guest')
    vendor=Path('/sys/class/dmi/id/sys_vendor').read_text().strip()
    product=Path('/sys/class/dmi/id/product_name').read_text().strip()
    require(vendor=='Microsoft Corporation' and product=='Virtual Machine','Expected an isolated Hyper-V guest')

@contextlib.contextmanager
def guest_lock(lab):
    require_guest();token(lab)
    import fcntl
    fd=os.open(RUN_ROOT/('openpilot-vm-recovery-'+lab+'.lock'),os.O_RDWR|os.O_CREAT|os.O_NOFOLLOW,0o600)
    try:
        require(stat.S_ISREG(os.fstat(fd).st_mode),'lab lock is not a regular file')
        fcntl.flock(fd,fcntl.LOCK_EX|fcntl.LOCK_NB)
        yield
    finally:os.close(fd)

def volume(path):
    require(path.is_absolute() and path.resolve()==path and path.is_dir() and not path.is_symlink(),'volume must be a canonical existing mount point')
    require(os.path.ismount(path) and path!=Path('/'),'volume must be a separate mounted data filesystem')
    result=subprocess.run(['findmnt','-n','-o','FSTYPE,UUID','--target',str(path)],capture_output=True,text=True,timeout=5,check=True)
    values=result.stdout.split()
    require(len(values)==2 and values[0]=='ext4','expected ext4 with a filesystem UUID')
    return {'uuid':str(uuid.UUID(values[1])),'device':path.stat().st_dev}

def sync_dir(path):
    fd=os.open(path,os.O_RDONLY|os.O_DIRECTORY|os.O_NOFOLLOW)
    try:os.fsync(fd)
    finally:os.close(fd)

def write_new(path,data):
    fd=os.open(path,os.O_WRONLY|os.O_CREAT|os.O_EXCL|os.O_NOFOLLOW,0o600)
    try:
        view=memoryview(data)
        while view:
            written=os.write(fd,view);require(written>0,'short baseline write');view=view[written:]
        os.fsync(fd)
    finally:os.close(fd)

def provision(src,dst,lab):
    require_guest();token(lab)
    identities=[volume(src),volume(dst)]
    require(identities[0]['device']!=identities[1]['device'] and identities[0]['uuid']!=identities[1]['uuid'],'two distinct data filesystems required')
    for root in (src,dst):require({p.name for p in root.iterdir()}<={'lost+found'},'provision requires an empty disposable data filesystem')
    for root,identity in zip((src,dst),identities):
        write_new(root/MARKER,json.dumps({'schema':'openpilot-vm-volume/v1','lab_token':lab,'filesystem_uuid':identity['uuid']}).encode())
        sync_dir(root)
    return {'outcome':'prepared','lab_token':lab,'volumes':[v['uuid'] for v in identities]}

def owned_volumes(src,dst,lab):
    require_guest();token(lab);values=[volume(src),volume(dst)]
    require(values[0]['device']!=values[1]['device'] and values[0]['uuid']!=values[1]['uuid'],'cross-filesystem roots required')
    for root,identity in zip((src,dst),values):
        marker=read_json(root/MARKER)
        require(marker.get('schema')=='openpilot-vm-volume/v1' and marker.get('lab_token')==lab and marker.get('filesystem_uuid')==identity['uuid'],'data-volume ownership/UUID mismatch')
    return values

def prepare(src,dst,lab,case,phase,fixture):
    token(case);require(phase in PHASES,'unknown checkpoint')
    identities=owned_volumes(src,dst,lab)
    require(fixture.is_file() and not fixture.is_symlink(),'missing private fixture')
    left=src/case;right=dst/case
    require(not left.exists() and not right.exists(),'case directories must be new')
    left.mkdir(mode=0o700);right.mkdir(mode=0o700)
    write_new(left/'source.bin',NEW);write_new(right/'source.bin',OLD)
    manifest={'schema':'openpilot-vm-case/v1','lab_token':lab,'case_id':case,'phase':phase,
              'source_volume':str(src),'destination_volume':str(dst),'filesystem_uuids':[v['uuid'] for v in identities],
              'source':str(left/'source.bin'),'target':str(right/'source.bin'),
              'new_sha256':sha_bytes(NEW),'old_sha256':sha_bytes(OLD),'new_size':len(NEW),'old_size':len(OLD),
              'fixture_sha256':sha_bytes(fixture.read_bytes()),'preparation_boot_id':boot_id()}
    write_new(left/'case.json',json.dumps(manifest,sort_keys=True).encode())
    for path in (left,right,src,dst):sync_dir(path)
    return manifest

def validate_manifest(path):
    m=read_json(path);require(m.get('schema')=='openpilot-vm-case/v1','unknown case schema')
    lab=token(m['lab_token']);case=token(m['case_id']);require(m['phase'] in PHASES,'unknown checkpoint')
    src=Path(m['source_volume']);dst=Path(m['destination_volume'])
    identities=owned_volumes(src,dst,lab)
    require(m['filesystem_uuids']==[v['uuid'] for v in identities],'mounted data filesystems changed')
    require(path==src/case/'case.json' and path.resolve()==path,'manifest is outside its owned source case')
    require(m['source']==str(src/case/'source.bin') and m['target']==str(dst/case/'source.bin'),'case paths differ from owned roots')
    require(m['new_sha256']==sha_bytes(NEW) and m['old_sha256']==sha_bytes(OLD) and m['new_size']==len(NEW) and m['old_size']==len(OLD),'payload contract mismatch')
    for root in (src/case,dst/case):require(root.is_dir() and not root.is_symlink(),'case directory substituted')
    return m

def file_hash(path):
    try:fd=os.open(path,os.O_RDONLY|os.O_NOFOLLOW|os.O_NOATIME|os.O_NONBLOCK)
    except FileNotFoundError:return None
    try:
        metadata=os.fstat(fd);require(stat.S_ISREG(metadata.st_mode) and metadata.st_size<=max(len(NEW),len(OLD)),'unexpected recovered object type/size')
        digest=hashlib.sha256()
        while True:
            data=os.read(fd,65536)
            if not data:break
            digest.update(data)
        return digest.hexdigest()
    finally:os.close(fd)

def evaluate(m,source_hash,target_hash):
    phase=m['phase'];require(phase in PHASES,'unknown checkpoint')
    before=phase in PHASES[:3]
    allowed_target={m['old_sha256']} if before else ({m['old_sha256'],m['new_sha256']} if phase=='before-destination-sync' else {m['new_sha256']})
    allowed_source={None} if phase=='after-source-sync' else ({None,m['new_sha256']} if phase=='after-source-unlink' else {m['new_sha256']})
    return {'outcome':'passed' if source_hash in allowed_source and target_hash in allowed_target else 'failed',
            'source_sha256':source_hash,'target_sha256':target_hash,'allowed_source_sha256':sorted(allowed_source,key=lambda v:v or ''),'allowed_target_sha256':sorted(allowed_target)}

def read_exact(stream,count,deadline):
    data=b''
    while len(data)<count:
        wait=deadline-time.monotonic();require(wait>0 and select.select([stream],[],[],wait)[0],'fixture protocol timeout')
        part=os.read(stream.fileno(),count-len(data));require(part,'fixture protocol EOF');data+=part
    return data

def run_to_checkpoint(m,fixture,timeout=30):
    require(m['phase'] in PHASES,'unknown checkpoint')
    require(sha_bytes(fixture.read_bytes())==m['fixture_sha256'],'private fixture hash changed')
    require(file_hash(Path(m['source']))==m['new_sha256'] and file_hash(Path(m['target']))==m['old_sha256'],'case baseline changed before arm')
    notify_read,notify_write=os.pipe();process=None
    env=dict(os.environ,OPVM_PHASE=m['phase'],OPVM_NOTIFY_FD=str(notify_write),OPVM_SOURCE=m['source'],OPVM_TARGET=m['target'],
             OPVM_SOURCE_DIR=str(Path(m['source']).parent),OPVM_DEST_DIR=str(Path(m['target']).parent),
             OPVM_STAGE_FILE=str(Path(m['target']).parent/'.openpilot-stage-*'/'payload'),OPVM_STAGE_DIR=str(Path(m['target']).parent/'.openpilot-stage-*'))
    try:
        process=subprocess.Popen([str(fixture),'--jobs'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,env=env,pass_fds=(notify_write,))
        os.close(notify_write);notify_write=-1;deadline=time.monotonic()+timeout
        typ,size,ident=HEAD.unpack(read_exact(process.stdout,12,deadline));require(typ==1 and size==16,'invalid HELLO')
        magic,version,_,_=struct.unpack('<IIII',read_exact(process.stdout,size,deadline))
        require(magic==0x31505357 and version==8,'wrong private fixture protocol')
        body=struct.pack('<IIII',1,2,1,0)
        for path in (str(Path(m['target']).parent),m['source']):
            raw=os.fsencode(path);body+=struct.pack('<II',len(raw),0)+raw
        process.stdin.write(HEAD.pack(10,len(body),1)+body);process.stdin.flush();continued=False
        while True:
            wait=deadline-time.monotonic();require(wait>0,'checkpoint timeout')
            ready=select.select([notify_read,process.stdout],[],[],wait)[0];require(ready,'checkpoint timeout')
            if notify_read in ready:
                raw=os.read(notify_read,4096);require(raw and len(raw)<4096 and raw.endswith(b'\n'),'invalid checkpoint notification')
                event=json.loads(raw,object_pairs_hook=unique);require(event['phase']==m['phase'] and event['fixture_version']==1,'wrong checkpoint')
                while time.monotonic()<deadline:
                    pid,status=os.waitpid(process.pid,os.WNOHANG|os.WUNTRACED)
                    if pid:require(os.WIFSTOPPED(status) and os.WSTOPSIG(status)==signal.SIGSTOP,'fixture did not stop at checkpoint');break
                    time.sleep(.005)
                else:raise RecoveryError('fixture stop was not observed')
                return process,event
            typ,size,ident=HEAD.unpack(read_exact(process.stdout,12,deadline));require(ident==1 and size<=1048576,'invalid fixture frame')
            data=read_exact(process.stdout,size,deadline)
            require(typ==11 and size>=48,'unexpected event or failed transfer before checkpoint')
            state=PROGRESS.unpack(data[:48])[2];require(state<3,'transfer completed without checkpoint')
            if state==2 and not continued:
                process.stdin.write(HEAD.pack(14,0,1));process.stdin.flush();continued=True
    except BaseException:
        if process is not None:
            if process.poll() is None:process.kill()
            process.wait(timeout=5)
            for stream in (process.stdin,process.stdout,process.stderr):stream.close()
        raise
    finally:
        os.close(notify_read)
        if notify_write>=0:os.close(notify_write)

def emit(event,serial):
    data=PREFIX+json.dumps(event,sort_keys=True,separators=(',',':')).encode()+b'\n'
    require(len(data)<16384,'oversized serial event')
    fd=os.open(serial,os.O_WRONLY|os.O_NOCTTY|os.O_NOFOLLOW)
    try:
        require(stat.S_ISCHR(os.fstat(fd).st_mode),'serial endpoint must be a character device')
        view=memoryview(data)
        while view:
            written=os.write(fd,view);require(written>0,'short serial write');view=view[written:]
    finally:os.close(fd)
    print(data.decode(),end='',flush=True)

def execute(args):
    if args.action=='provision':print(json.dumps(provision(args.source_volume,args.destination_volume,args.lab_token),indent=2));return 0
    if args.action=='prepare':print(json.dumps(prepare(args.source_volume,args.destination_volume,args.lab_token,args.case_id,args.phase,args.fixture),indent=2));return 0
    m=validate_manifest(args.manifest);current=boot_id()
    base={'schema':'openpilot-vm-event/v1','lab_token':m['lab_token'],'case_id':m['case_id'],'phase':m['phase'],'boot_id':current,'fixture_sha256':m['fixture_sha256']}
    if args.action=='arm':
        require(current==m['preparation_boot_id'],'prepare a fresh case in the current boot before arming')
        process,event=run_to_checkpoint(m,args.fixture)
        try:
            emit(dict(base,event='checkpoint',worker_stopped=True,checkpoint=event),args.serial)
            while True:signal.pause()
        finally:
            if process.poll() is None:process.kill()
            process.wait(timeout=5)
            for stream in (process.stdin,process.stdout,process.stderr):stream.close()
    require(current!=m['preparation_boot_id'],'same-boot inspection is not VM recovery evidence')
    result=evaluate(m,file_hash(Path(m['source'])),file_hash(Path(m['target'])))
    candidates=[p.name for p in Path(m['target']).parent.iterdir() if p.name.startswith('.openpilot-stage-')]
    emit(dict(base,event='recovered',observation_only=True,staging_candidates=sorted(candidates),**result),args.serial)
    return 0 if result['outcome']=='passed' else 1

def main():
    parser=argparse.ArgumentParser(description=__doc__);sub=parser.add_subparsers(dest='action',required=True)
    for name in ('provision','prepare'):
        cmd=sub.add_parser(name);cmd.add_argument('--source-volume',type=Path,required=True);cmd.add_argument('--destination-volume',type=Path,required=True);cmd.add_argument('--lab-token',required=True)
        if name=='prepare':cmd.add_argument('--case-id',required=True);cmd.add_argument('--phase',choices=PHASES,required=True);cmd.add_argument('--fixture',type=Path,required=True)
    for name in ('arm','verify'):
        cmd=sub.add_parser(name);cmd.add_argument('--manifest',type=Path,required=True);cmd.add_argument('--serial',default='/dev/ttyS0')
        if name=='arm':cmd.add_argument('--fixture',type=Path,required=True)
    args=parser.parse_args()
    try:
        lab=args.lab_token if args.action in ('provision','prepare') else read_json(args.manifest)['lab_token']
        with guest_lock(lab):return execute(args)
    except (OSError,ValueError,KeyError,subprocess.SubprocessError) as exc:
        print(json.dumps({'outcome':'error','error':str(exc)}),file=sys.stderr);return 2

if __name__=='__main__':sys.exit(main())
