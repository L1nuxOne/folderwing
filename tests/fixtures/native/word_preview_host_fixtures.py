"""Owned synthetic document and archive fixtures, including VML text boxes."""
import shutil,subprocess,sys,zipfile
from pathlib import Path
root=Path(sys.argv[1]);root.mkdir(parents=True,exist_ok=True)
fixture=Path(__file__).parents[1]/'word'/'synthetic.docx'
textbox=root/'textbox.docx'
subprocess.run([sys.executable,str(Path(__file__).parents[1]/'word'/'make_textbox_fixture.py'),str(textbox)],check=True)
shutil.copyfile(fixture,root/'sample.docx')
with zipfile.ZipFile(root/'word-preview.zip','w',zipfile.ZIP_DEFLATED) as z:
 z.write(fixture,'sample.docx')
 z.write(textbox,'textbox.docx')
