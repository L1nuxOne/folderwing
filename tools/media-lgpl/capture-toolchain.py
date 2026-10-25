"""Inside the locked builder, preserve installed package/runtime notice records."""
from pathlib import Path
import argparse,shutil
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--work-directory',type=Path,default=Path('/work'));a=p.parse_args()
root=a.work_directory;shutil.copyfile('/toolchain-record/packages.tsv',root/'toolchain-packages.tsv')
notices=root/'toolchain-notices';notices.mkdir(exist_ok=True)
for line in Path('/toolchain-record/packages.tsv').read_text().splitlines():
    name=line.split('\t',1)[0];file=Path('/usr/share/doc')/name/'copyright'
    if file.is_file():shutil.copyfile(file,notices/(name+'.copyright'))
print('Preserved',len(list(notices.iterdir())),'toolchain copyright records')
