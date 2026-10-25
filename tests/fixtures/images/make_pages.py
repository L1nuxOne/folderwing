"""Deterministic, uncompressed multipage TIFF fixtures; standard library only."""
from pathlib import Path
import struct
import zlib
root=Path(__file__).parent

def profile():
    data=(root/'profile.png').read_bytes();off=8
    while off+12<=len(data):
        n=struct.unpack_from('>I',data,off)[0];kind=data[off+4:off+8];chunk=data[off+8:off+8+n];off+=12+n
        if kind==b'iCCP':return zlib.decompress(chunk[chunk.index(b'\0')+2:])
    raise ValueError('Expected embedded PNG colour profile')

def tiff(name,pages):
    # All IFDs precede their external values/strips. Each page has its own metadata.
    tables=[]
    for w,h,rgb,orientation,camera,icc,broken in pages:
        fields=[(256,4,1,w),(257,4,1,h),(258,3,3,struct.pack('<HHH',8,8,8)),(259,3,1,1),(262,3,1,2),
                (272,2,len(camera)+1,camera.encode()+b'\0'),(273,4,1,0),(274,3,1,orientation),
                (277,3,1,3),(278,4,1,h),(279,4,1,w*h*3),(284,3,1,1),(306,2,20,b'2026:10:14 12:00:00\0')]
        if icc:fields.append((34675,7,len(icc),icc))
        tables.append(sorted(fields))
    offsets=[];offset=8
    for fields in tables:offsets.append(offset);offset+=2+len(fields)*12+4
    extras=bytearray();ifds=[]
    for i,fields in enumerate(tables):
        w,h,rgb,orientation,camera,icc,broken=pages[i];entries=[]
        for tag,typ,count,value in fields:
            if tag==273:
                value=offset+len(extras) if broken!='pixels' else 0x7ffffff0
                extras.extend(bytes(rgb)*(w*h) if broken!='size' else bytes(rgb))
            if isinstance(value,bytes):
                if len(value)>4:v=struct.pack('<I',offset+len(extras));extras.extend(value)
                else:v=value.ljust(4,b'\0')
            else:v=struct.pack('<I',value)
            entries.append(struct.pack('<HHI',tag,typ,count)+v)
        ifds.append(struct.pack('<H',len(fields))+b''.join(entries)+struct.pack('<I',offsets[i+1] if i+1<len(pages) else 0))
    (root/name).write_bytes(b'II\x2a\x00'+struct.pack('<I',8)+b''.join(ifds)+extras)

red=(16,8,(240,20,40),1,'Red page',None,None)
green=(10,6,(20,200,40),6,'Green page',profile(),None)
blue=(8,12,(20,40,240),3,'Blue page',None,None)
tiff('pages.tiff',[red,green,blue])
tiff('page_oversize.tiff',[red,(300000,1,(0,0,0),1,'Oversize page',None,'size'),blue])
tiff('page_corrupt.tiff',[red,(10,6,(0,0,0),1,'Damaged page',None,'pixels'),blue])
