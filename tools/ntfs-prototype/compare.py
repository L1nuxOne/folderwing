"""Compare prototype and WizTree exports by NTFS record identity, not row order."""
import argparse,csv,json,time
from array import array
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('own',type=Path);p.add_argument('wiztree',type=Path);p.add_argument('--output',type=Path,required=True);args=p.parse_args()
t=time.monotonic();own_info=json.loads(args.own.with_suffix('.json').read_text(encoding='utf-8-sig'));slots=own_info['slots'];missing=(1<<64)-1
own_size=array('Q',[missing])*slots;own_alloc=array('Q',[0])*slots;own_all_size=array('Q',[0])*slots;own_all_alloc=array('Q',[0])*slots;own_kind=bytearray(slots);own_names=array('I',[0])*slots
own_unknown=[];own_rows=0;own_file_rows=0
with args.own.open(encoding='utf-8-sig',newline='') as stream:
 rows=csv.reader(stream);header=next(rows)
 for row in rows:
  ident=int(row[3]);own_rows+=1;directory=int(row[5]);own_kind[ident]=2 if directory else 1
  own_size[ident]=int(row[1]);own_alloc[ident]=int(row[2]);own_all_size[ident]=int(row[7]);own_all_alloc[ident]=int(row[8]);own_names[ident]+=1
  if not directory:
   own_file_rows+=1
   if row[10]=='0' and len(own_unknown)<20:own_unknown.append({'path':row[0],'record':ident,'size':int(row[1])})
print('Prototype rows read',own_rows,flush=True)
wiz_size=array('Q',[missing])*slots;wiz_alloc=array('Q',[0])*slots;wiz_names=array('I',[0])*slots
counts={'wiz_file_rows':0,'wiz_folder_rows':0,'common_file_records':0,'logical_equal':0,'allocated_equal':0,'both_equal':0,'logical_matches_all_streams':0,'allocation_matches_all_streams':0,'wiz_only_file_rows':0,'own_only_file_records':0,'logical_mismatch':0,'allocated_mismatch':0,'name_count_mismatch':0}
samples=[];wiz_extra=[];root=None;seen=set()
with args.wiztree.open(encoding='utf-8-sig',newline='') as stream:
 rows=csv.reader(stream)
 for row in rows:
  if row and row[0]=='File Name':header=row;break
 columns={name:i for i,name in enumerate(header)}
 for row in rows:
  if not row:continue
  path=row[columns['File Name']];ident=int(row[columns['MFTRECNO']])&((1<<48)-1)
  if path.endswith('\\'):
   counts['wiz_folder_rows']+=1
   if ident==5:root=dict(zip(header,row))
   continue
  counts['wiz_file_rows']+=1
  if ident>=slots or own_kind[ident]!=1:
   counts['wiz_only_file_rows']+=1
   if len(wiz_extra)<20:wiz_extra.append({'path':path,'record':ident,'size':int(row[columns['Size']])})
   continue
  size=int(row[columns['Size']]);allocated=int(row[columns['Allocated']]);wiz_size[ident]=size;wiz_alloc[ident]=max(wiz_alloc[ident],allocated);wiz_names[ident]+=1
  if ident not in seen and len(samples)<500:
   samples.append((ident,path))
  seen.add(ident)
print('WizTree rows read',counts['wiz_file_rows']+counts['wiz_folder_rows'],flush=True)
# Retain paths for mismatching records in a second bounded pass, avoiding a multi-million path dictionary.
logical_bad=set();allocated_bad=set();name_bad=set()
for ident in range(slots):
 if own_kind[ident]!=1:continue
 if wiz_size[ident]==missing:counts['own_only_file_records']+=1;continue
 counts['common_file_records']+=1;size_equal=own_size[ident]==wiz_size[ident];allocated_equal=own_alloc[ident]==wiz_alloc[ident]
 counts['logical_equal']+=size_equal;counts['allocated_equal']+=allocated_equal;counts['both_equal']+=size_equal and allocated_equal
 if not size_equal:
  counts['logical_mismatch']+=1;logical_bad.add(ident)
  counts['logical_matches_all_streams']+=own_all_size[ident]==wiz_size[ident]
 if not allocated_equal:
  counts['allocated_mismatch']+=1;allocated_bad.add(ident)
  counts['allocation_matches_all_streams']+=own_all_alloc[ident]==wiz_alloc[ident]
 if own_names[ident]!=wiz_names[ident]:counts['name_count_mismatch']+=1;name_bad.add(ident)
examples=[];mismatch_ids=logical_bad|allocated_bad|name_bad;stored=set()
with args.own.open(encoding='utf-8-sig',newline='') as stream:
 rows=csv.reader(stream);next(rows)
 for row in rows:
  ident=int(row[3])
  if ident not in mismatch_ids or ident in stored:continue
  if len(examples)<60:examples.append({'path':row[0],'record':ident,'ours_size':own_size[ident],'wiz_size':wiz_size[ident],'ours_all_stream_size':own_all_size[ident],'ours_allocated':own_alloc[ident],'wiz_allocated':wiz_alloc[ident],'ours_all_stream_allocated':own_all_alloc[ident],'ours_names':own_names[ident],'wiz_names':wiz_names[ident],'attributes':int(row[9])})
  stored.add(ident)
  if len(stored)==len(mismatch_ids):break
result={'schema':'openpilot-mft-wiztree-comparison/v1','live_independent_scans':True,'source_files':{'own':str(args.own),'wiztree':str(args.wiztree)},'own_file_rows':own_file_rows,'own_folder_rows':own_rows-own_file_rows,'counts':counts,'root_wiztree':root,'unknown_data_examples':own_unknown,'wiz_only_examples':wiz_extra,'mismatch_examples':examples,'elapsed_s':time.monotonic()-t}
args.output.write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(counts,indent=2));print('Unknown data examples:',json.dumps(own_unknown));print('Evidence',args.output)
