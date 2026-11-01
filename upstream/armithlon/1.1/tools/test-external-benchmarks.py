#!/usr/bin/env python3
"""Local-only tests for benchmark adapters. No network, no GPU, no Geekbench required."""
import importlib.util
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

SCRIPT = Path(__file__).with_name('external-benchmarks.py')
spec = importlib.util.spec_from_file_location('external_benchmarks', SCRIPT)
eb = importlib.util.module_from_spec(spec)
spec.loader.exec_module(eb)


class AdapterTests(unittest.TestCase):
    def test_glmark2_score(self):
        self.assertEqual(eb.parse_score('glmark2', '[glmark2] Score: 2189\n'), 2189)

    def test_vkmark_score(self):
        self.assertEqual(eb.parse_score('vkmark', '[vkmark] Score: 1120\n'), 1120)

    def test_last_score_wins(self):
        self.assertEqual(eb.parse_score('vkmark', 'Score: 12\nScore: 23\n'), 23)

    def test_scene_fps_not_score(self):
        self.assertIsNone(eb.parse_score('glmark2', '[build] 125 fps\n'))
        self.assertIsNone(eb.parse_score('vkmark', 'score: 12.5\n'))

    def test_skipped_if_missing(self):
        with tempfile.TemporaryDirectory() as tmp, patch.object(eb, 'find_binary', return_value=None):
            res = eb.run_tool('glmark2', Path(tmp))
            self.assertEqual(res['status'], 'SKIP')

    def test_exec_and_raw_log(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            mock = root / 'glmark2'
            mock.write_text('#!/bin/sh\necho "[glmark2] Score: 123"\n', encoding='utf8')
            mock.chmod(0o700)
            with patch.object(eb, 'find_binary', return_value=mock):
                res = eb.run_tool('glmark2', root / 'output')
            self.assertEqual(res['status'], 'PASS')
            self.assertEqual(res['score'], 123)
            self.assertIn('Score: 123', (root / 'output' / 'glmark2.log').read_text())
            self.assertTrue(res['binary_sha256'])

    def test_nonzero_is_error(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            mock = root / 'vkmark'
            mock.write_text('#!/bin/sh\necho "Score: 999"\nexit 5\n', encoding='utf8')
            mock.chmod(0o700)
            with patch.object(eb, 'find_binary', return_value=mock):
                res = eb.run_tool('vkmark', root / 'output')
            self.assertEqual(res['status'], 'ERROR')

    def test_malformed_json_not_overwritten(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'external-results.json'
            path.write_text('BROKEN', encoding='utf8')
            with self.assertRaises(ValueError):
                eb.store(Path(tmp), eb.new_entry('glmark2', 'PASS', score=3))
            self.assertEqual(path.read_text(), 'BROKEN')

    def test_manual_geekbench_import(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            rc = eb.cli(['import-geekbench', '--cpu-single', '2000', '--cpu-multi', '7777',
                         '--gpu-metal', '34802', '--source', 'https://browser.geekbench.com/v7/cpu/123',
                         '--out', str(out)])
            self.assertEqual(rc, 0)
            data = json.loads((out / 'external-results.json').read_text())
            self.assertEqual(data['results']['geekbench7']['status'], 'MANUAL')
            self.assertFalse(data['results']['geekbench7']['verified'])
            self.assertIn('<html', (out / 'benchmarks.html').read_text())
            self.assertEqual(data['results']['geekbench7']['metrics']['gpu_metal'], 34802)

    def test_manual_invalid_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.assertEqual(eb.cli(['import-geekbench', '--gpu-metal', '0',
                                    '--source', 'my GUI', '--out', tmp]), 2)
            self.assertFalse((Path(tmp) / 'external-results.json').exists())

    def test_report_does_not_compare_mixed_scores(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            eb.store(out, eb.new_entry('glmark2', 'PASS', score=199))
            eb.store(out, eb.new_entry('vkmark', 'SKIP', 'not installed'))
            report = (out / 'EXTERNAL-REPORT.md').read_text()
            self.assertIn('Nicht vergleichbar', report)
            self.assertIn('glmark2', report)
            self.assertIn('vkmark', report)
            d = json.loads((out / 'external-results.json').read_text())
            self.assertEqual(d['results']['glmark2']['score'], 199)
            self.assertEqual(d['results']['vkmark']['status'], 'SKIP')


if __name__ == '__main__':
    unittest.main(verbosity=2)
