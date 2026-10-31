"""Produce local synthetic variants from a checked-in, non-user workbook."""
from pathlib import Path
import sys,zipfile
out=Path(sys.argv[1]);out.mkdir(parents=True,exist_ok=True)
base=Path(__file__).with_name('synthetic.xlsx')
def make(name,fn):
 with zipfile.ZipFile(base) as src,zipfile.ZipFile(out/name,'w') as dst:
  for entry in src.infolist():
   content=fn(entry.filename,src.read(entry.filename))
   dst.writestr(entry,content)
def unchanged(n,b):return b
make('base.xlsx',unchanged)
# A harmless inert VBA part verifies that macro-enabled containers are read as data.
with zipfile.ZipFile(base) as src,zipfile.ZipFile(out/'macro.xlsm','w') as dst:
 for entry in src.infolist():dst.writestr(entry,src.read(entry.filename))
 dst.writestr('xl/vbaProject.bin',b'INERT SYNTHETIC VBA PAYLOAD')
def shared(n,b):
 if n=='xl/worksheets/sheet1.xml':
  b=b.replace(b'</sheetData>',b'<row r="5"><c r="A5"><f t="shared" si="7" ref="A5:A6">B5+1</f><v>9</v></c></row><row r="6"><c r="A6"><f t="shared" si="7"/><v>10</v></c><c r="B6" t="str"><f>""</f><v/></c><c r="C6"><f t="dataTable"/><v>4</v></c></row></sheetData>')
 return b
make('shared.xlsx',shared)
def inflated(n,b):return b.replace(b'uniqueCount="11"',b'uniqueCount="1000000"') if n=='xl/sharedStrings.xml' else b
make('inflated.xlsx',inflated)
def dtd(n,b):return b.replace(b'<worksheet ',b'<!DOCTYPE worksheet [<!ENTITY example "ignored">]><worksheet ') if n=='xl/worksheets/sheet1.xml' else b
make('dtd.xlsx',dtd)
def wrong(n,b):return b.replace(b'r="A2"',b'r="XFE2"',1) if n=='xl/worksheets/sheet1.xml' else b
make('bad-column.xlsx',wrong)
def external(n,b):return b.replace(b'Target="worksheets/sheet1.xml"',b'Target="https://evil.invalid/sheet.xml" TargetMode="External"',1) if n=='xl/_rels/workbook.xml.rels' else b
make('external.xlsx',external)
def moved(n,b):
 if n=='xl/_rels/workbook.xml.rels':return b.replace(b'worksheets/sheet1.xml',b'worksheets/unusual.xml')
 return b
# Repackage a worksheet under a non-default relationship target.
with zipfile.ZipFile(base) as src,zipfile.ZipFile(out/'moved.xlsx','w') as dst:
 for entry in src.infolist():
  data=moved(entry.filename,src.read(entry.filename));name='xl/worksheets/unusual.xml' if entry.filename=='xl/worksheets/sheet1.xml' else entry.filename
  dst.writestr(name,data)
print('Workbook variants created')

# A fake relationship suffix must not override the library's actual worksheet
# or shared-string relationship (which could desynchronize formulas or bounds).
with zipfile.ZipFile(base) as src,zipfile.ZipFile(out/'spoofed.xlsx','w') as dst:
 for entry in src.infolist():
  data=src.read(entry.filename)
  if entry.filename=='xl/_rels/workbook.xml.rels':
   data=data.replace(b'</Relationships>',b'<Relationship Id="rId1" Type="https://evil.invalid/worksheet" Target="worksheets/spoof.xml"/><Relationship Id="fake" Type="https://evil.invalid/sharedStrings" Target="fake.xml"/></Relationships>')
  dst.writestr(entry,data)
 dst.writestr('xl/worksheets/spoof.xml',b'<worksheet/>')
 dst.writestr('xl/fake.xml',b'<sst/>')

def longer(n,b):
 if n=='xl/worksheets/sheet1.xml':
  rows=[]
  for r in range(4,141):
   formula=('<f>A%d+1</f>'%r) if r%10==0 else ''
   rows.append(('<row r="%d"><c r="A%d">%s<v>%d</v></c></row>'%(r,r,formula,r)).encode())
  b=b.replace(b'</sheetData>',b''.join(rows)+b'</sheetData>')
 return b
make('long.xlsx',longer)
