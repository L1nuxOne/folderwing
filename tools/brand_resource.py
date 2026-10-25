"""Resource link input shared by the focused WSL build drivers (Zig only)."""
from pathlib import Path
import subprocess

def build_brand_resource(root:Path,out:Path,zig:str)->str:
    def windows(path):return 'C:/'+str(path).split('/mnt/c/',1)[1]
    target=out/'folderwing-brand.o'
    result=subprocess.run([zig,'rc','/fo',windows(target),windows(root/'app/folderwing.rc')],capture_output=True,text=True)
    (out/'folderwing-resource.log').write_text(result.stdout+result.stderr)
    if result.returncode:raise RuntimeError('Folderwing resource compilation failed: '+result.stdout+result.stderr)
    return windows(target)
