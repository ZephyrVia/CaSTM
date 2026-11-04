"""Negative checks against real short-run artifacts; run explicitly with a fixture."""
import csv
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'scripts'))
from verify_memory_soak import verify_run


@unittest.skipUnless(os.environ.get('STM_MEMORY_SOAK_RESULTS'), 'short-run artifacts not supplied')
class SoakReportTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        source = Path(os.environ['STM_MEMORY_SOAK_RESULTS'])
        for name in ('native-result.json', 'native-samples.csv'):
            shutil.copyfile(source / name, self.root / name)

    def change_result(self, change):
        path = self.root / 'native-result.json'
        data = json.loads(path.read_text())
        change(data)
        path.write_text(json.dumps(data))

    def test_real_short_run(self):
        self.assertEqual(verify_run(self.root, 'native', 3)['phases'], 3)

    def test_rejects_checksum_mismatch(self):
        self.change_result(lambda d: d['phases'][0].update(checksum=0))
        with self.assertRaises(AssertionError): verify_run(self.root, 'native', 3)

    def test_rejects_missing_reclamation(self):
        self.change_result(lambda d: d['phases'][0].update(reclaimed=0))
        with self.assertRaises(AssertionError): verify_run(self.root, 'native', 3)

    def test_rejects_unreleased_witness(self):
        self.change_result(lambda d: d['phases'][1].update(release_checks=0))
        with self.assertRaises(AssertionError): verify_run(self.root, 'native', 3)

    def test_short_run_cannot_be_called_long(self):
        with self.assertRaises(AssertionError): verify_run(self.root, 'native', 600)

    def test_rejects_missing_churn_checkpoint(self):
        path = self.root / 'native-samples.csv'
        with path.open(newline='') as stream:
            reader = csv.DictReader(stream)
            fields = reader.fieldnames
            rows = list(reader)
        rows = [r for r in rows if r['event'] != 'batch_joined']
        with path.open('w', newline='') as stream:
            writer = csv.DictWriter(stream, fieldnames=fields)
            writer.writeheader(); writer.writerows(rows)
        with self.assertRaises(AssertionError): verify_run(self.root, 'native', 3)


if __name__ == '__main__':
    unittest.main()
