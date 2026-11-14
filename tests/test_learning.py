import argparse
import csv
import importlib.util
from pathlib import Path
import tempfile
import unittest

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('learning', ROOT / 'tools/apprendimento/apprendimento.py')
learning = importlib.util.module_from_spec(spec)
spec.loader.exec_module(learning)


class LearningTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name) / 'synthetic.csv'
        self.rows = [dict(timestamp=1705000000 + i * 30, online=1,
                          room=20 + i * .01, outside=10, target=21,
                          internalTarget=23.5, rpm=500, power=1, turbo=0,
                          eco=0, mode='4', fan='A') for i in range(100)]

    def write_rows(self, rows):
        with self.path.open('w', newline='', encoding='utf-8') as stream:
            writer = csv.DictWriter(stream, fieldnames=learning.FIELDS)
            writer.writeheader()
            writer.writerows(rows)

    def test_forecast_horizon_and_feature_order(self):
        self.write_rows(self.rows)
        x, y, stamps, rooms = learning.load_examples(self.path)
        self.assertEqual(x.shape, (70, 12))
        np.testing.assert_allclose(y, .2)
        np.testing.assert_allclose(x[:, 5], .1)
        np.testing.assert_allclose(x[:, 6:10], [[1, 1, 0, 0]] * len(x))

    def test_missing_samples_exclude_windows_crossing_gap(self):
        self.write_rows(self.rows[:40] + self.rows[44:])
        x, y, stamps, rooms = learning.load_examples(self.path)
        self.assertGreater(len(x), 0)
        # History endpoints may be up to 35 seconds later than the requested
        # five-minute mark. Windows inside that tolerance need not cross the gap.
        gap_start, gap_end = self.rows[39]['timestamp'], self.rows[44]['timestamp']
        self.assertFalse(any(stamp - 300 + 35 < gap_end and stamp + 600 > gap_start for stamp in stamps))

    def test_control_change_excludes_labels_with_changed_future(self):
        self.rows[50]['target'] = 23
        self.write_rows(self.rows)
        x, y, stamps, rooms = learning.load_examples(self.path)
        changed = self.rows[50]['timestamp']
        self.assertFalse(any(stamp <= changed <= stamp + 600 for stamp in stamps))

    def test_merge_deduplicates_overlapping_downloads(self):
        self.write_rows(self.rows)
        result = Path(self.temp.name) / 'merged.csv'
        learning.merge(argparse.Namespace(files=[str(self.path), str(self.path)], output=str(result)))
        with result.open(newline='', encoding='utf-8') as stream:
            merged = list(csv.DictReader(stream))
        self.assertEqual(len(merged), 100)

    def test_too_little_data_does_not_export_a_model(self):
        self.write_rows(self.rows)
        output = Path(self.temp.name) / 'model'
        with self.assertRaises(SystemExit):
            learning.train(argparse.Namespace(csv=str(self.path), output=str(output)))
        self.assertFalse((output / 'modello-wemos.txt').exists())
