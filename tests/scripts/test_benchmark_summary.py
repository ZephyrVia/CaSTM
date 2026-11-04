import csv
from pathlib import Path
import sys
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'scripts'))
from summarize_stm_benchmark import summarize_rows


class SummaryTests(unittest.TestCase):
    def test_retry_policies_cannot_be_averaged_together(self):
        base = dict(backend='occ', threads='20', read_ratio='50', contention='high', valid='1',
                    abort_rate='0', p50_us='1', p99_us='2')
        rows = [dict(base, backoff='none', throughput_tps='10'),
                dict(base, backoff='jitter', throughput_tps='1000')]
        summary = {row['backoff']: row for row in summarize_rows(rows)}
        self.assertEqual(len(summary), 2)
        self.assertEqual(summary['none']['throughput_mean_tps'], '10.000000')
        self.assertEqual(summary['jitter']['throughput_mean_tps'], '1000.000000')

    def test_archived_summary_is_unchanged(self):
        data = Path(__file__).resolve().parents[2] / 'docs/performance/data'
        with (data / 'stm-raw.csv').open(newline='') as f:
            actual = summarize_rows(csv.DictReader(f))
        with (data / 'stm-summary.csv').open(newline='') as f:
            expected = list(csv.DictReader(f))
        self.assertEqual(actual, expected)
