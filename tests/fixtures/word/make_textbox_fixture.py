"""Create an owned Word document with VML text boxes and contrasting text."""
import io,sys,zipfile
from pathlib import Path
base=Path(__file__).with_name('synthetic.docx')
out=Path(sys.argv[1])
box='''<w:p><w:r><w:pict><v:shape style="position:absolute;width:400pt;height:300pt" filled="f" stroked="f"><v:textbox inset="0,0,0,0"><w:txbxContent>
<w:p><w:r><w:rPr><w:color w:val="FFFFFF"/></w:rPr><w:t>White text on an omitted background</w:t></w:r></w:p>
<w:p><w:pPr><w:shd w:fill="202020"/></w:pPr><w:r><w:rPr><w:color w:val="FFFFFF"/></w:rPr><w:t>White text on a dark background</w:t></w:r></w:p>
<w:p><w:r><w:rPr><w:color w:val="008000"/></w:rPr><w:t>Readable green text</w:t></w:r></w:p>
<w:p><w:r><w:t>Box math \\(x+</w:t></w:r><w:r><w:rPr><w:b/></w:rPr><w:t>y\\)</w:t></w:r></w:p>
</w:txbxContent></v:textbox></v:shape></w:pict></w:r></w:p>'''.encode()
with zipfile.ZipFile(base) as src,zipfile.ZipFile(out,'w',zipfile.ZIP_DEFLATED) as dst:
 for e in src.infolist():
  data=src.read(e.filename)
  if e.filename=='word/document.xml':
   data=data.replace(b'<w:document ',b'<w:document xmlns:v="urn:schemas-microsoft-com:vml" ',1)
   data=data.replace(b'<w:sectPr/>',box+b'<w:sectPr/>')
  dst.writestr(e,data)
