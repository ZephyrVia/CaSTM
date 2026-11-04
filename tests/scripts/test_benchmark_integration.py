"""Short counter/serialization checks; these are not performance measurements."""
import csv
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

BINARY = os.environ.get('STM_BENCHMARK_BIN')


@unittest.skipUnless(BINARY, 'set STM_BENCHMARK_BIN to enable binary integration checks')
class BenchmarkIntegrationTests(unittest.TestCase):
    def test_per_worker_counts_reconcile_with_csv_for_all_backends(self):
        for backend in ('mutex', 'shared_mutex', 'occ', 'ww'):
            with self.subTest(backend=backend), tempfile.TemporaryDirectory() as tmp:
                report = Path(tmp) / 'diagnostics.json'
                command = [BINARY, '--backend', backend, '--threads', '2', '--read-ratio', '90',
                           '--contention', 'low', '--warmup-ms', '10', '--measure-ms', '30',
                           '--diagnostics-json', str(report), '--csv']
                if backend == 'occ':
                    command += ['--backoff', 'jitter']
                result = subprocess.run(command, capture_output=True, text=True, timeout=20)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                row = list(csv.DictReader(result.stdout.splitlines()))[0]
                data = json.loads(report.read_text())
                self.assertTrue(data['valid'])
                for phase in ('warmup', 'measure'):
                    counts = data[phase]
                    self.assertEqual(counts['attempts'], counts['committed'] + counts['aborts'])
                    self.assertEqual(sum(counts['abort_causes'].values()), counts['aborts'])
                    self.assertEqual(sum(counts['retry_histogram']), counts['committed'] + counts['failed'])
                    for field in ('attempts', 'aborts', 'committed', 'failed', 'writes'):
                        self.assertEqual(counts[field], sum(w[phase][field] for w in data['workers']))
                self.assertEqual(int(row['attempts']), data['measure']['attempts'])
                self.assertEqual(int(row['aborts']), data['measure']['aborts'])
                self.assertEqual(int(row['expected_checksum']), data['warmup']['writes'] + data['measure']['writes'])
                self.assertEqual(row['expected_checksum'], row['final_checksum'])

    def test_unwritable_diagnostics_is_reported_as_invalid(self):
        with tempfile.TemporaryDirectory() as tmp:
            result = subprocess.run([BINARY, '--backend', 'mutex', '--warmup-ms', '0',
                                     '--measure-ms', '1', '--diagnostics-json',
                                     str(Path(tmp) / 'missing' / 'file.json'), '--csv'],
                                    capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 1)
            row = list(csv.DictReader(result.stdout.splitlines()))[0]
            self.assertEqual(row['valid'], '0')
            self.assertEqual(row['failure_reason'], 'diagnostics_io')
