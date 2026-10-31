"""Create only owned, disposable XML files and an archive for the host test."""
import sys
import zipfile
from pathlib import Path
root = Path(sys.argv[1])
root.mkdir(parents=True, exist_ok=True)
sample = '<?xml version="1.0"?><root xmlns="urn:test"><item id="1">alpha<![CDATA[beta]]></item><!--note--></root>'
(root / 'sample.xml').write_text(sample, encoding='utf-8')
with zipfile.ZipFile(root / 'xml-preview.zip', 'w', zipfile.ZIP_DEFLATED) as archive:
    archive.writestr('sample.xml', sample)
