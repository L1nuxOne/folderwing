#!/usr/bin/env python3
"""Read-only staging candidates, run in Linux/WSL (including /mnt/c folders).

Names are hints, never proof of ownership, abandonment or completeness. No file
contents are opened. Walks use pinned directory FDs and never follow child links
or enter a staging candidate. The report is an observation, not a cleanup token.
"""
import argparse
import errno
import json
import os
import re
import stat
import sys

PATTERNS = (
    ('linux-workspace', re.compile(r'\.openpilot-stage-[0-9a-f]{32}\Z')),
    ('windows-copy', re.compile(r'\.openpilot-copy-[0-9a-fA-F]{32}\.tmp\Z')),
    ('windows-stream', re.compile(r'\.openpilot-host-[0-9]+-[0-9]+\.tmp\Z')),
)
FLAGS = os.O_RDONLY | getattr(os, 'O_DIRECTORY', 0) | getattr(os, 'O_NOFOLLOW', 0) | getattr(os, 'O_CLOEXEC', 0)


def identity(st):
    kind = ('directory' if stat.S_ISDIR(st.st_mode) else
            'file' if stat.S_ISREG(st.st_mode) else
            'symlink' if stat.S_ISLNK(st.st_mode) else 'other')
    return dict(type=kind, device=st.st_dev, inode=st.st_ino, size=st.st_size,
                mode=stat.S_IMODE(st.st_mode), uid=st.st_uid, mtime_ns=st.st_mtime_ns)


def review(root, depth=0, max_entries=10000, max_candidates=1000):
    if sys.platform != 'linux':
        raise RuntimeError('Run in Linux/WSL; use /mnt/c/... for Windows folders.')
    if not 0 <= depth <= 16 or not 1 <= max_entries <= 1000000 or not 1 <= max_candidates <= 10000:
        raise ValueError('depth must be 0..16, entries 1..1000000, candidates 1..10000')
    root = os.path.abspath(os.fspath(root))
    report = dict(schema='openpilot-staging-review/v1', root=root,
                  policy='keep-for-review', ownership='unverified', activity='unknown',
                  candidates=[], errors=[], entries=0, truncated=False,
                  depth=depth, max_entries=max_entries, max_candidates=max_candidates)

    def error(path, exc):
        report['errors'].append(dict(path=path, errno=exc.errno))

    def walk(fd, relative, remaining):
        # scandir owns its duplicate, leaving our anchor valid for openat/stat.
        with os.scandir(fd) as entries:
            for entry in entries:
                if report['entries'] >= max_entries or len(report['candidates']) >= max_candidates:
                    report['truncated'] = True
                    return
                report['entries'] += 1
                name = entry.name
                path = os.path.join(relative, name)
                try:
                    st = os.stat(name, dir_fd=fd, follow_symlinks=False)
                    family = next((kind for kind, pattern in PATTERNS if pattern.fullmatch(name)), None)
                    if family:
                        report['candidates'].append(dict(path=path, family=family, **identity(st)))
                    elif remaining and stat.S_ISDIR(st.st_mode):
                        child = os.open(name, FLAGS, dir_fd=fd)
                        try:
                            held = os.fstat(child)
                            if (held.st_dev, held.st_ino) != (st.st_dev, st.st_ino):
                                raise OSError(errno.ESTALE, 'Directory changed')
                            walk(child, path, remaining-1)
                        finally:
                            os.close(child)
                except OSError as exc:
                    error(path, exc)
                if report['truncated']:
                    return

    try:
        fd = os.open(root, FLAGS)
        try:
            report['root_identity'] = identity(os.fstat(fd))
            walk(fd, '', depth)
        finally:
            os.close(fd)
    except OSError as exc:
        error('', exc)
    report['candidates'].sort(key=lambda item: item['path'])
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('folder', help='Explicit directory to inspect; no drive-wide scan by default')
    parser.add_argument('--depth', type=int, default=0, help='Child directory depth, default 0, maximum 16')
    parser.add_argument('--max-entries', type=int, default=10000)
    parser.add_argument('--max-candidates', type=int, default=1000)
    args = parser.parse_args()
    try:
        report = review(args.folder, args.depth, args.max_entries, args.max_candidates)
    except (RuntimeError, ValueError) as exc:
        parser.error(str(exc))
    # ASCII escaping preserves raw Linux filename bytes and prevents filenames
    # from injecting terminal controls. Stdout only; never create a report in
    # the inspected directory or overwrite a candidate.
    print(json.dumps(report, ensure_ascii=True, indent=2))
    return 1 if report['errors'] or report['truncated'] else 0


if __name__ == '__main__':
    sys.exit(main())
