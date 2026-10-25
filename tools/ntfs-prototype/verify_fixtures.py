"""Check frozen owned-file handle metadata against both independent MFT exports."""
import argparse,csv,json
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('directory',type=Path);args=p.parse_args();base=args.directory
expected=json.loads((base/'FIXTURES.json').read_text(encoding='utf-8-sig'));wanted={x['path'].casefold():x for x in expected};own={};wiz={};wiz_by_id={};own_by_id={};ids={x['record'] for x in expected}
with (base/'own.csv').open(encoding='utf-8-sig',newline='') as f:
 for row in csv.DictReader(f):
  if row['File Name'].casefold() in wanted:own[row['File Name'].casefold()]=row;own_by_id[int(row['MFT Record'])]=row
with (base/'wiztree.csv').open(encoding='utf-8-sig',newline='') as f:
 rows=csv.reader(f)
 for row in rows:
  if row and row[0]=='File Name':header=row;break
 for values in rows:
  if not values:continue
  row=dict(zip(header,values));ident=int(row['MFTRECNO'])
  if ident in ids:
   if ident not in wiz_by_id or int(row['Allocated'])>int(wiz_by_id[ident]['Allocated']):wiz_by_id[ident]=row
   if row['File Name'].casefold() in wanted:wiz[row['File Name'].casefold()]=row
checks=[]
for path,item in wanted.items():
 a=own.get(path);b=wiz.get(path)
 facts={'exported by prototype':a is not None,'exported by WizTree':b is not None}
 if a:
  facts.update({'record identity':int(a['MFT Record'])==item['record'],'record sequence':int(a['Sequence'])==item['reference']>>48,'current logical length':int(a['Size'])==item['size'],'current default-stream allocation':int(a['Allocated'])==item['allocated'],'known metadata':a['Data Known']=='1'})
 # The deliberately tiny resident fixture has no separate data clusters.
 # FileStandardInfo reports the resident value rounded to eight bytes; this is
 # a distinct quantity from MFT/WizTree physical allocation (already in $MFT).
 if a and item['path'].endswith('resident-β.txt'):
  witness=json.loads((base/'RESIDENT_ALLOCATION.json').read_text(encoding='utf-8-sig'))
  facts.pop('current default-stream allocation')
  facts['resident file has no separate allocated clusters']=int(a['Allocated'])==0
  facts['independent Windows resident allocation differs explicitly']=item['allocated']==witness['standard_allocation']==8 and witness['compression_allocation']==witness['compressed_file_size']==item['size']==3
 if a and b:
  facts['WizTree logical length']=int(b['Size'])==int(a['Size'])
  facts['WizTree allocation including alternate streams']=int(wiz_by_id[item['record']]['Allocated'])==int(a['All Stream Allocated'])
 for label,ok in facts.items():checks.append({'file':Path(item['path'].replace('\\','/')).name,'check':label,'passed':ok})
normal=next(x for x in expected if x['path'].endswith('normal.bin'));linked=next(x for x in expected if x['path'].endswith('hardlink.bin'))
checks.append({'file':'normal.bin / hardlink.bin','check':'one file identity for two literal names','passed':normal['reference']==linked['reference'] and normal['path'].casefold() in own and linked['path'].casefold() in own})
checks.append({'file':'normal.bin:extra','check':'alternate stream consumes additional physical allocation','passed':int(own_by_id[normal['record']]['All Stream Allocated'])>int(own_by_id[normal['record']]['Allocated'])})
result={'schema':'openpilot-mft-fixtures/v1','outcome':'passed' if all(x['passed'] for x in checks) else 'failed','checks':len(checks),'failures':[x for x in checks if not x['passed']],'details':checks,'analysis_source_sha256':__import__('hashlib').sha256(Path(__file__).read_bytes()).hexdigest(),'resident_oracle':'controlled three-byte resident fixture; no separate data clusters, independently agrees with WizTree; Windows standard/compression byte quantities retained as separate witnesses'}
(base/'FIXTURE_VERIFICATION.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps({k:v for k,v in result.items() if k!='details'},indent=2));raise SystemExit(0 if result['outcome']=='passed' else 1)
