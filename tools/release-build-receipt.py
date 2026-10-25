"""Capture and seal actual source/build inputs for a release candidate."""
import argparse
from pathlib import Path
from release_common import digest,source_snapshot,seal_build,need
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--source-key',action='store_true');p.add_argument('--build-directory',type=Path);p.add_argument('--compiler',type=Path);p.add_argument('--expected-source-key');a=p.parse_args()
if a.source_key:print(digest(source_snapshot()))
else:
    need(a.expected_source_key==digest(source_snapshot()),'Source changed during build; keep the failed attempt and rebuild the final tree')
    result=seal_build(a.build_directory,a.compiler);print('Sealed',len(result['binaries']),'release binaries; source digest',result['source_digest'])
