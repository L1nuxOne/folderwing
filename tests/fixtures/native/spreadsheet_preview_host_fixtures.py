"""Owned synthetic spreadsheet and archive fixtures."""
import shutil,sys,zipfile
from pathlib import Path
root=Path(sys.argv[1]);root.mkdir(parents=True,exist_ok=True)
fixture=Path(__file__).parents[1]/'spreadsheet'/'synthetic.xlsx'
shutil.copyfile(fixture,root/'sample.xlsx')
with zipfile.ZipFile(root/'spreadsheet-preview.zip','w',zipfile.ZIP_DEFLATED) as z:z.write(fixture,'sample.xlsx')
