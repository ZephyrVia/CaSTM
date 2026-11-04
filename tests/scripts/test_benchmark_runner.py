import csv
import io
from pathlib import Path
import subprocess
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'scripts'))
from run_stm_benchmark import normalize_result, parse_binary_row, failed_row


class RunnerTests(unittest.TestCase):
    def result(self, code=0, stderr='', **updates):
        row = dict(backend='occ', threads='4', attempts='100', committed='1', valid='1',
                   failure_reason='none', error='')
        row.update(updates)
        out = io.StringIO()
        writer = csv.DictWriter(out, fieldnames=list(row))
        writer.writeheader()
        writer.writerow(row)
        return normalize_result(subprocess.CompletedProcess([], code, out.getvalue(), stderr),
                                'occ', 4, 50, 'high', 1)

    def test_nonzero_exit_overrides_valid_csv(self):
        row = self.result(code=2)
        self.assertEqual(row['valid'], '0')
        self.assertEqual(row['failure_reason'], 'process_exit')
        self.assertEqual(row['process_returncode'], '2')

    def test_signal_is_distinguished(self):
        row = self.result(code=-11)
        self.assertEqual(row['failure_reason'], 'signal')
        self.assertEqual(row['valid'], '0')

    def test_reason_and_multiline_error_survive_stderr(self):
        row = self.result(code=1, valid='0', failure_reason='exception',
                          error='bad, "value"\nnext line', stderr='extra detail')
        self.assertEqual(row['failure_reason'], 'exception')
        self.assertEqual(row['error'], 'bad, "value"\nnext line | extra detail')

    def test_malformed_and_empty_output_are_invalid(self):
        for value in ['', 'not,csv\na,b\n', 'backend,threads,attempts,committed,valid\nocc,4\n']:
            self.assertIsNone(parse_binary_row(value))
            row = normalize_result(subprocess.CompletedProcess([], 0, value, ''),
                                   'occ', 4, 50, 'high', 1)
            self.assertEqual(row['valid'], '0')
            self.assertEqual(row['failure_reason'], 'parse_error')

    def test_timeout_has_no_invented_counters(self):
        row = failed_row('occ', 4, 50, 'high', 1, 'timeout', 'timeout')
        self.assertEqual(row['valid'], '0')
        self.assertEqual(row['attempts'], '')
        self.assertEqual(row['failure_reason'], 'timeout')


if __name__ == '__main__':
    unittest.main()
