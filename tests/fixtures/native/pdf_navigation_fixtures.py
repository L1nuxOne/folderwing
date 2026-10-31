"""Generate only synthetic PDF navigation fixtures in a new owned directory."""
from pathlib import Path
import argparse,json,hashlib

def make_pdf(path,outline=False,cycle=False):
    objects=[]
    def obj(data=b''):
        objects.append(data);return len(objects)
    catalog=obj();pages=obj();font=obj(b'<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>')
    children=[]
    for i in range(12):
        stream=f'BT /F1 24 Tf 72 720 Td (Synthetic section {i+1}) Tj ET\nBT /F1 12 Tf 72 680 Td (Owned PDF navigation fixture. No personal documents.) Tj ET'.encode()
        content=obj(b'<< /Length '+str(len(stream)).encode()+b' >>\nstream\n'+stream+b'\nendstream')
        page=obj(f'<< /Type /Page /Parent {pages} 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 {font} 0 R >> >> /Contents {content} 0 R >>'.encode());children.append(page)
    objects[pages-1]=f'<< /Type /Pages /Count 12 /Kids [{" ".join(f"{p} 0 R" for p in children)}] >>'.encode()
    extra=''
    if outline:
        root=obj();first=obj();nested=obj();last=obj();external=obj()
        objects[root-1]=f'<< /Type /Outlines /First {first} 0 R /Last {external} 0 R /Count 4 >>'.encode()
        objects[first-1]=f'<< /Title (Opening) /Parent {root} 0 R /First {nested} 0 R /Last {nested} 0 R /Count 1 /Next {last} 0 R /Dest [{children[0]} 0 R /XYZ 72 720 null] >>'.encode()
        title=('\ufeffÉquations 日本語').encode('utf-16-be').hex()
        objects[nested-1]=f'<< /Title <{title}> /Parent {first} 0 R /Dest [{children[4]} 0 R /XYZ 72 720 null] >>'.encode()
        objects[last-1]=f'<< /Title (Conclusion) /Parent {root} 0 R /Prev {first} 0 R /Next {external} 0 R /A << /S /GoTo /D [{children[9]} 0 R /XYZ 72 720 null] >> >>'.encode()
        objects[external-1]=f'<< /Title (External link must not be followed) /Parent {root} 0 R /Prev {last} 0 R {f"/Next {first} 0 R" if cycle else ""} /A << /S /URI /URI (https://invalid.example/no-network) >> >>'.encode()
        extra=f'/Outlines {root} 0 R'
    objects[catalog-1]=f'<< /Type /Catalog /Pages {pages} 0 R {extra} >>'.encode()
    data=bytearray(b'%PDF-1.7\n');offsets=[0]
    for i,value in enumerate(objects,1):offsets.append(len(data));data.extend(f'{i} 0 obj\n'.encode()+value+b'\nendobj\n')
    xref=len(data);data.extend(f'xref\n0 {len(objects)+1}\n0000000000 65535 f \n'.encode())
    for offset in offsets[1:]:data.extend(f'{offset:010} 00000 n \n'.encode())
    data.extend(f'trailer << /Size {len(objects)+1} /Root {catalog} 0 R >>\nstartxref\n{xref}\n%%EOF\n'.encode());path.write_bytes(data)

def main():
    p=argparse.ArgumentParser();p.add_argument('--output',type=Path,required=True);args=p.parse_args();out=args.output;out.mkdir(parents=True,exist_ok=False)
    make_pdf(out/'plain.pdf');make_pdf(out/'outline.pdf',True);make_pdf(out/'cycle.pdf',True,True)
    manifest={'schema':'folderwing-pdf-navigation-fixtures/v1','personal_documents':False,'files':{p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in out.glob('*.pdf')}}
    (out/'FIXTURES.json').write_text(json.dumps(manifest,indent=2)+'\n')
if __name__=='__main__':main()
