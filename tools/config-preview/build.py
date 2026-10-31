"""Rebuild the checked-in browser bundle from locked npm inputs in a temporary directory."""
from pathlib import Path
import argparse, hashlib, os, shutil, subprocess, tempfile

here=Path(__file__).resolve().parent
target=here.parents[1]/'app/markdown/vendor/config-parsers.js'
args=argparse.ArgumentParser(description=__doc__)
args.add_argument('--write',action='store_true',help='replace the checked-in bundle with the rebuilt output')
options=args.parse_args()
with tempfile.TemporaryDirectory(prefix='folderwing-config-bundle-') as temporary:
    work=Path(temporary)
    for name in ('package.json','package-lock.json','entry.js'):shutil.copyfile(here/name,work/name)
    npm=[os.environ.get('COMSPEC','cmd.exe'),'/d','/c','npm.cmd'] if os.name=='nt' else ['npm']
    subprocess.run(npm+['ci','--ignore-scripts','--no-audit','--no-fund'],cwd=work,check=True)
    executable=work/'node_modules/esbuild/bin/esbuild'
    output=work/'config-parsers.js'
    subprocess.run(['node',str(executable),'entry.js','--bundle','--platform=browser','--format=iife','--target=es2020','--minify','--outfile='+str(output)],cwd=work,check=True)
    data=output.read_bytes()
    if options.write:target.write_bytes(data)
    elif not target.is_file() or target.read_bytes()!=data:raise SystemExit('Rebuilt bundle differs from the checked-in asset')
    print('Verified bundle SHA-256:',hashlib.sha256(data).hexdigest())
