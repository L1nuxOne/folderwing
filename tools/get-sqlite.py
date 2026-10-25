#!/usr/bin/env python3
"""Fetch the pinned SQLite amalgamation for the WSL index experiment.

Not called by build.ps1. Uses the SHA3-256 published on sqlite.org/download.html.
An existing destination must match every source hash; it is never overwritten.
"""
import argparse
import hashlib
import io
import json
from pathlib import Path
import tempfile
import urllib.request
import zipfile

VERSION = "3.54.0"
URL = "https://sqlite.org/2026/sqlite-amalgamation-3540000.zip"
ARCHIVE_SHA3 = "7b670a62fdfbd672b75fef004cb703c8a3e87d3a5cc7d675b4a08337004a2d93"
SOURCE_HASHES = {
    "sqlite3.c": "2da161e310ef0aba3465f22a4d12b8a43723236f0942fa76e8aac523c3317243",
    "sqlite3.h": "cd20a2877bc82b94e36c1093b727dd87cfa3c1d581aac6e240925535cbd90166",
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dest", type=Path, default=Path(__file__).resolve().parents[1] / "third_party/sqlite")
    args = parser.parse_args()
    dest = args.dest.resolve()
    if dest.exists():
        for name, expected in SOURCE_HASHES.items():
            if hashlib.sha256((dest / name).read_bytes()).hexdigest() != expected:
                raise SystemExit(f"Existing SQLite source does not match pin: {dest / name}")
        print(f"SQLite {VERSION} sources verified: {dest}")
        return
    with urllib.request.urlopen(URL, timeout=60) as response:
        data = response.read(8 * 1024**2 + 1)
    if hashlib.sha3_256(data).hexdigest() != ARCHIVE_SHA3:
        raise SystemExit("SQLite archive checksum mismatch")
    dest.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".sqlite-stage-", dir=dest.parent) as tmp:
        stage = Path(tmp) / "sqlite"
        stage.mkdir()
        with zipfile.ZipFile(io.BytesIO(data)) as archive:
            for name, expected in SOURCE_HASHES.items():
                source = archive.read("sqlite-amalgamation-3540000/" + name)
                if hashlib.sha256(source).hexdigest() != expected:
                    raise SystemExit(f"SQLite source checksum mismatch: {name}")
                (stage / name).write_bytes(source)
        (stage / "VERSION.openpilot.json").write_text(json.dumps({
            "version": VERSION, "url": URL, "archive_sha3_256": ARCHIVE_SHA3,
            "source_sha256": SOURCE_HASHES,
        }, indent=2) + "\n")
        stage.rename(dest)
    print(f"SQLite {VERSION} downloaded and verified: {dest}")


if __name__ == "__main__":
    main()
