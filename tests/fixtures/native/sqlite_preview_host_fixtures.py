"""Create only synthetic, disposable databases and a ZIP in the supplied directory."""
import sqlite3
import sys
import zipfile
from pathlib import Path

root = Path(sys.argv[1])
root.mkdir(parents=True, exist_ok=True)
for name, wal in [('sample.sqlite3', False), ('wal.sqlite', True)]:
    path = root / name
    with sqlite3.connect(str(path)) as db:
        if wal:
            db.execute('PRAGMA journal_mode=WAL')
        db.execute('CREATE TABLE items (id INTEGER, label TEXT)')
        db.executemany('INSERT INTO items VALUES (?, ?)', [(1, 'first'), (2, 'second')])
    if wal:
        header = path.read_bytes()[:20]
        assert header[18] == 2 and header[19] == 2
with zipfile.ZipFile(root / 'sqlite-preview.zip', 'w', zipfile.ZIP_DEFLATED) as archive:
    archive.write(root / 'sample.sqlite3', 'sample.sqlite3')
    archive.write(root / 'wal.sqlite', 'wal.sqlite')
