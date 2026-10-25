"""Install the exact GNU PDFium release input; preserve old inputs on migration."""
import argparse,importlib.util,json,os,uuid
from pathlib import Path
from release_common import plain
HERE=Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location('folderwing_bootstrap',HERE/'bootstrap-release.py')
bootstrap=importlib.util.module_from_spec(spec);spec.loader.exec_module(bootstrap)
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--archive',type=Path);p.add_argument('--base-url');p.add_argument('--migrate-existing',action='store_true');a=p.parse_args()
 if os.name!='nt':raise ValueError('Use Windows Python')
 root=HERE.parent;pin=bootstrap.read(root/'tools/release-inputs.json')['dependencies']['pdfium'].copy()
 base=a.base_url or os.environ.get('FOLDERWING_PDFIUM_BASE_URL')
 if base:pin['url']=base.rstrip('/')+'/'+pin['archive_name']
 archive=a.archive or (Path(os.environ['FOLDERWING_PDFIUM_ARCHIVE']) if os.environ.get('FOLDERWING_PDFIUM_ARCHIVE') else None)
 if archive is None:archive=bootstrap.download(pin,root/'build/downloads')
 plain(archive)
 if bootstrap.sha(archive)!=pin['sha256'] or archive.stat().st_size!=pin['bytes']:raise ValueError('PDFium archive checksum/size mismatch; existing input retained')
 dest=root/'third_party/pdfium';backup=None
 if dest.exists():
  receipt=dest/'.folderwing-bootstrap.json';current=bootstrap.read(receipt) if receipt.exists() else None
  if not current or current.get('archive_sha256')!=pin['sha256']:
   legacy=dest/'bin/pdfium.dll'
   if not a.migrate_existing or not legacy.is_file() or bootstrap.sha(plain(legacy))!='1f2aee4286b8cf0413ab4562b7121aa454f9cfd9afd6ca8609bcef80927b3f80':raise ValueError('Existing non-current PDFium retained. Only the recognized legacy input can migrate with --migrate-existing.')
   files={f.relative_to(dest).as_posix():bootstrap.sha(plain(f)) for f in dest.rglob('*') if f.is_file()}
   backup=root/'build/dependency-backups'/('pdfium-legacy-'+uuid.uuid4().hex);backup.mkdir(parents=True)
   (backup/'BACKUP.json').write_text(json.dumps({'files':files,'reason':'reversible recognized legacy PDFium migration'},indent=2)+'\n');dest.rename(backup/'input')
 try:result=bootstrap.install('pdfium',pin,root/'build/downloads',archive)
 except Exception:
  if backup and not dest.exists():(backup/'input').rename(dest)
  raise
 print(json.dumps({'outcome':'installed','runtime_sha256':result['files']['bin/pdfium.dll'],'legacy_backup':str(backup) if backup else None,'version':pin['version']},indent=2))
if __name__=='__main__':
 try:main()
 except (ValueError,OSError,KeyError) as e:raise SystemExit(str(e))
