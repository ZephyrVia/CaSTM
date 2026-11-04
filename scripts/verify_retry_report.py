#!/usr/bin/env python3
"""Check archived retry metrics, worker counters, checksums and artifact hashes."""
import argparse
from collections import Counter
import csv
import gzip
import hashlib
import json
from pathlib import Path

from summarize_stm_benchmark import summarize_rows


def verify(root: Path) -> dict:
    provenance = json.loads((root / 'provenance.json').read_text())
    for name, expected in provenance['sha256'].items():
        assert hashlib.sha256((root / name).read_bytes()).hexdigest() == expected, name
    with (root / 'raw.csv').open(newline='') as stream:
        rows = list(csv.DictReader(stream))
    with (root / 'summary.csv').open(newline='') as stream:
        summary = list(csv.DictReader(stream))
    assert summarize_rows(rows) == summary, 'summary differs from raw data'
    with gzip.open(root / 'diagnostics.jsonl.gz', 'rt') as stream:
        entries = [json.loads(line) for line in stream]
    diagnostics = {entry['path']: entry['data'] for entry in entries}
    assert len(entries) == len(diagnostics) == len(rows)
    reasons = Counter()
    for row in rows:
        data = diagnostics[row['diagnostics_file']]
        assert data['valid'] == (row['valid'] == '1')
        for field in ('backend', 'backoff', 'contention', 'failure_reason', 'failure_phase'):
            assert data[field] == row[field], (row['diagnostics_file'], field)
        for field in ('threads', 'read_ratio', 'run', 'failure_worker'):
            assert data[field] == int(row[field])
        assert len(data['workers']) == data['threads']
        for phase in ('warmup', 'measure'):
            total = data[phase]
            assert total['attempts'] == total['committed'] + total['aborts']
            assert sum(total['abort_causes'].values()) == total['aborts']
            assert sum(total['retry_histogram']) == total['committed'] + total['failed']
            for field in ('attempts', 'aborts', 'committed', 'failed', 'writes'):
                assert total[field] == sum(w[phase][field] for w in data['workers'])
            for i in range(65):
                assert total['retry_histogram'][i] == sum(w[phase]['retry_histogram'][i] for w in data['workers'])
            for cause in total['abort_causes']:
                assert total['abort_causes'][cause] == sum(w[phase]['abort_causes'][cause] for w in data['workers'])
        for csv_field, field in (('attempts', 'attempts'), ('committed', 'committed'),
                                 ('aborts', 'aborts'), ('failed_logical', 'failed'),
                                 ('max_retries', 'max_retries')):
            assert int(row[csv_field]) == data['measure'][field]
        assert int(row['expected_checksum']) == data['warmup']['writes'] + data['measure']['writes']
        assert row['expected_checksum'] == row['final_checksum']
        if row['valid'] != '1':
            reasons[row['failure_reason']] += 1
            if row['failure_reason'] == 'retry_limit':
                phase = data[row['failure_phase']]
                assert phase['failed'] > 0
                assert phase['max_retries'] == data['max_attempts'] - 1
    return dict(runs=len(rows), valid=sum(r['valid'] == '1' for r in rows), invalid_reasons=dict(reasons))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    print(json.dumps(verify(args.directory), indent=2))


if __name__ == '__main__':
    main()
