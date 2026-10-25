"""Record the candidate's PE imports, exports, hardening and CRT link members."""
from __future__ import annotations
import argparse, hashlib, json, re, struct
from pathlib import Path

def need(ok,msg):
 if not ok:raise ValueError(msg)
class PE:
 def __init__(self,path):
  self.data=path.read_bytes();d=self.data;need(d[:2]==b'MZ','Not a PE image');nt=self.u32(0x3c);need(d[nt:nt+4]==b'PE\0\0','Invalid PE signature')
  self.machine=self.u16(nt+4);count=self.u16(nt+6);opt=nt+24;need(self.u16(opt)==0x20b,'Expected x64 PE32+');self.flags=self.u16(opt+70);self.dirs=[struct.unpack_from('<II',d,opt+112+i*8) for i in range(16)]
  self.sections=[];start=opt+self.u16(nt+20)
  for i in range(count):
   off=start+i*40;vsize,va,size,raw=struct.unpack_from('<IIII',d,off+8);self.sections.append((va,max(vsize,size),raw,size))
 def u16(self,x):return struct.unpack_from('<H',self.data,x)[0]
 def u32(self,x):return struct.unpack_from('<I',self.data,x)[0]
 def offset(self,rva):
  for va,size,raw,physical in self.sections:
   if va<=rva<va+size:
    delta=rva-va;need(delta<physical and raw+delta<len(self.data),'Unbacked PE RVA');return raw+delta
  raise ValueError('Unmapped PE RVA: '+hex(rva))
 def string(self,rva):
  off=self.offset(rva);end=self.data.find(b'\0',off);need(end>=off,'Unterminated PE name');return self.data[off:end].decode('ascii')
 def imports(self):
  start,size=self.dirs[1];need(start and size,'No imports');off=self.offset(start);rows={}
  for i in range(4096):
   original,timestamp,forward,name,first=struct.unpack_from('<IIIII',self.data,off+i*20)
   if not any((original,timestamp,forward,name,first)):return rows
   dll=self.string(name);thunk=self.offset(original or first);symbols=[]
   for k in range(65536):
    n=struct.unpack_from('<Q',self.data,thunk+k*8)[0]
    if not n:break
    symbols.append('#'+str(n&0xffff) if n>>63 else self.string(n+2))
   else:raise ValueError('Oversized import list')
   rows[dll]=symbols
  raise ValueError('Oversized import directory')
 def exports(self):
  rva,size=self.dirs[0]
  if not rva:return []
  off=self.offset(rva);count=self.u32(off+24);names=self.offset(self.u32(off+32));need(count<100000,'Oversized export directory');return sorted(self.string(self.u32(names+4*i)) for i in range(count))

def audit(dll,map_file,baseline=None,commands=None):
 pe=PE(dll);imports=pe.imports();exports=pe.exports();names={n.lower() for n in imports}
 need(pe.machine==0x8664,'Expected AMD64');need(pe.flags&0x40 and pe.flags&0x100,'ASLR/NX missing');need(pe.flags&0x4000,'CFG missing')
 need('vcruntime140.dll' in names or 'vcruntime140_1.dll' in names,'External VC runtime import missing')
 need('ucrtbase.dll' in names or any(n.startswith('api-ms-win-crt-') for n in names),'External UCRT import missing')
 missing=[];added=[]
 if baseline:
  old=set(PE(baseline).exports());missing=sorted(old-set(exports));added=sorted(set(exports)-old);need(not missing,'Previously exported PDFium APIs missing: '+', '.join(missing[:8]))
 text=map_file.read_text(errors='replace');members={}
 for lib,member in re.findall(r'(?im)(?:^|\s)(msvcrt|libcmt|libvcruntime|vcruntime|msvcprt|libcpmt|ucrt|libucrt|oldnames|delayimp)(?:\.lib)?:([^\s]+)',text):members.setdefault(lib.lower(),set()).add(member)
 static={lib:sorted(m for m in v if m.lower().endswith(('.obj','.o'))) for lib,v in members.items()};static={k:v for k,v in static.items() if v}
 compile_counts=None
 if commands:
  s=commands.read_text(encoding='utf-8-sig');need(not re.search(r'(?im)(?:^|\s)/MTd?(?:\s|$)',s),'Static CRT compile flag present');compile_counts={'dynamic_crt_commands':len(re.findall(r'(?im)(?:^|\s)/MD(?:\s|$)',s)),'static_crt_commands':0}
 return {'schema':'folderwing-pdfium-runtime-audit/v1','dll_sha256':hashlib.sha256(dll.read_bytes()).hexdigest(),'bytes':dll.stat().st_size,'imports':imports,'export_count':len(exports),'removed_exports':missing,'added_exports':added,'hardening':{'aslr':True,'nx':True,'cfg':True},'compile_flags':compile_counts,'crt_map_members':{k:sorted(v) for k,v in members.items()},'static_microsoft_archive_objects':static,'microsoft_static_glue_observed':bool(static),'external_runtime_imports_verified':True,'public_release_ready':False,'release_note':'Dynamic CRT imports do not by themselves establish that all Microsoft implementation code is absent; startup/header and map review remains required.'}

def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--dll',type=Path,required=True);p.add_argument('--map',type=Path,required=True);p.add_argument('--baseline',type=Path);p.add_argument('--commands',type=Path);p.add_argument('--output',type=Path,required=True);a=p.parse_args();need(not a.output.exists(),'Refuse to overwrite audit evidence');r=audit(a.dll,a.map,a.baseline,a.commands);a.output.write_text(json.dumps(r,indent=2)+'\n');print(json.dumps({k:r[k] for k in ['dll_sha256','bytes','export_count','hardening','compile_flags','microsoft_static_glue_observed','public_release_ready']},indent=2))
if __name__=='__main__':
 try:main()
 except (ValueError,OSError,struct.error) as e:raise SystemExit(str(e))
