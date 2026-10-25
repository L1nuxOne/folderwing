"""Keep-for-review safety: no mutation, no link traversal, bounded observations."""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

TOOL = Path(__file__).resolve().parents[1]/'tools/staging-review.py'
spec = importlib.util.spec_from_file_location('staging_review', TOOL)
review = importlib.util.module_from_spec(spec)
spec.loader.exec_module(review)
WORKSPACE = '.openpilot-stage-'+'a'*32
COPY = '.openpilot-copy-'+'B'*32+'.tmp'
STREAM = '.openpilot-host-123-4.tmp'


class StagingReview(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='openpilot_review_')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def test_all_families_are_unverified_and_untouched(self):
        stage = self.root/WORKSPACE; stage.mkdir(mode=0o700)
        (stage/'payload').write_bytes(b'partial data')
        for name in [COPY, STREAM, 'completed.bin', '.openpilot-copy-fake.tmp']:
            (self.root/name).write_bytes(b'keep these bytes')
        before = {p: (p.lstat().st_ino, p.lstat().st_mtime_ns, p.read_bytes())
                  for p in self.root.rglob('*') if p.is_file()}
        result = review.review(self.root, depth=3)
        self.assertEqual([p['path'] for p in result['candidates']], sorted([COPY, STREAM, WORKSPACE]))
        self.assertEqual((result['policy'], result['ownership'], result['activity']),
                         ('keep-for-review', 'unverified', 'unknown'))
        self.assertEqual(before, {p: (p.lstat().st_ino, p.lstat().st_mtime_ns, p.read_bytes()) for p in before})
        self.assertFalse(result['errors']); self.assertFalse(result['truncated'])

    def test_symlinks_are_observed_without_following_targets(self):
        outside = self.root/'outside'; outside.mkdir()
        (outside/COPY).write_bytes(b'foreign')
        scope = self.root/'scope'; scope.mkdir()
        (scope/'alias').symlink_to(outside, target_is_directory=True)
        (scope/WORKSPACE).symlink_to(outside, target_is_directory=True)
        result = review.review(scope, depth=16)
        self.assertEqual([(p['path'], p['type']) for p in result['candidates']], [(WORKSPACE, 'symlink')])
        self.assertTrue(review.review(scope/'alias')['errors'])
        self.assertEqual((outside/COPY).read_bytes(), b'foreign')

    def test_entry_candidate_and_depth_limits_report_incompleteness(self):
        for i in range(5): (self.root/('.openpilot-host-1-'+str(i)+'.tmp')).touch()
        result = review.review(self.root, max_entries=2)
        self.assertTrue(result['truncated']); self.assertEqual(result['entries'], 2)
        result = review.review(self.root, max_candidates=1)
        self.assertTrue(result['truncated']); self.assertEqual(len(result['candidates']), 1)
        child = self.root/'child'; child.mkdir(); (child/COPY).touch()
        self.assertEqual(len(review.review(self.root)['candidates']), 5)
        self.assertEqual(len(review.review(self.root, depth=1)['candidates']), 6)
        for value in [-1, 17]:
            with self.assertRaises(ValueError): review.review(self.root, depth=value)

    def test_substitution_between_stat_and_open_never_follows_link(self):
        child = self.root/'child'; child.mkdir()
        outside = self.root/'outside'; outside.mkdir(); (outside/COPY).touch()
        scope = self.root/'scope'; scope.mkdir(); child.rename(scope/'child')
        real_open = os.open
        def raced_open(path, flags, **kwargs):
            if path == 'child':
                (scope/'child').rename(self.root/'held')
                (scope/'child').symlink_to(outside, target_is_directory=True)
            return real_open(path, flags, **kwargs)
        with patch.object(review.os, 'open', raced_open):
            result = review.review(scope, depth=2)
        self.assertEqual(result['candidates'], []); self.assertEqual(len(result['errors']), 1)

    def test_cli_reports_errors_and_escapes_raw_names(self):
        folder = self.root/os.fsdecode(b'raw-\xff\n'); folder.mkdir()
        (folder/COPY).touch()
        cmd = [sys.executable, str(TOOL), str(self.root), '--depth', '1']
        result = subprocess.run(cmd, capture_output=True, check=True)
        body = json.loads(result.stdout)
        self.assertEqual(body['candidates'][0]['path'], str(folder.relative_to(self.root)/COPY))
        self.assertIn(b'\\udcff\\n', result.stdout)
        failed = subprocess.run([*cmd, '--max-entries', '1'], capture_output=True)
        self.assertEqual(failed.returncode, 1); self.assertTrue(json.loads(failed.stdout)['truncated'])
        failed = subprocess.run([sys.executable, str(TOOL), str(self.root/'missing')], capture_output=True)
        self.assertEqual(failed.returncode, 1); self.assertTrue(json.loads(failed.stdout)['errors'])


if __name__ == '__main__':
    unittest.main()
