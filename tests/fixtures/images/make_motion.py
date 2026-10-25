"""Deterministic SVG and GIF disposal fixtures; standard library only."""
from pathlib import Path
import struct
root = Path(__file__).parent
(root/'shapes.svg').write_text('''<svg xmlns="http://www.w3.org/2000/svg" width="16" height="8" viewBox="0 0 16 8"><rect width="8" height="8" fill="#f01428"/><rect x="8" width="8" height="4" fill="#14a0f0" fill-opacity="0.5"/></svg>''')
(root/'viewbox.svg').write_text('''<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 12"><path d="M0 0H24V12H0Z" fill="red"/></svg>''')
(root/'partial.svg').write_text('''<svg xmlns="http://www.w3.org/2000/svg" width="16" height="8"><rect width="16" height="8" fill="red"/><text x="0" y="8">Unsupported text</text><image href="https://example.invalid/never-fetch.png"/></svg>''')
(root/'doctype.svg').write_text('''<!DOCTYPE svg [<!ENTITY external SYSTEM "file:///C:/Windows/win.ini">]><svg xmlns="http://www.w3.org/2000/svg">&external;</svg>''')
(root/'oversize.svg').write_text('<svg xmlns="http://www.w3.org/2000/svg" width="999999" height="8"/>')
(root/'broken.svg').write_text('<svg><rect></svg>')
def frame(x,y,w,h,color,disposal,delay):
    # Clear before every literal keeps the LZW width at three bits.
    codes=[]
    for _ in range(w*h): codes.extend([4,color])
    codes.append(5)
    bits=sum(code<<(i*3) for i,code in enumerate(codes))
    payload=bits.to_bytes((len(codes)*3+7)//8,'little')
    sub=b''.join(bytes([len(payload[i:i+255])])+payload[i:i+255] for i in range(0,len(payload),255))+b'\0'
    return b'\x21\xf9\x04'+bytes([(disposal<<2)|1])+struct.pack('<H',delay)+b'\0\0'+b'\x2c'+struct.pack('<HHHHB',x,y,w,h,0)+b'\x02'+sub
header=b'GIF89a'+struct.pack('<HHBBB',6,4,0x81,0,0)+bytes([0,0,0,255,0,0,0,255,0,0,0,255])
loop=b'\x21\xff\x0bNETSCAPE2.0\x03\x01\x01\x00\x00'
data=header+loop+frame(0,0,6,4,1,1,12)+frame(1,1,2,2,2,3,7)+frame(3,1,2,2,3,2,9)+frame(0,0,1,1,2,1,11)+b'\x3b'
(root/'disposal.gif').write_bytes(data)
