#!/usr/bin/env python3
"""Reconcile soak CSV checkpoints, witnesses, payload lifetimes and durations."""
import argparse
import csv
import hashlib
import json
from pathlib import Path


PHASES = ('steady', 'slow_reader', 'thread_churn')


def verify_run(root: Path, mode: str, minimum_seconds: int) -> dict:
    result = json.loads((root / f'{mode}-result.json').read_text())
    assert result['allocator'] == mode and result['passed'], (mode, 'result invalid')
    assert [p['phase'] for p in result['phases']] == list(PHASES)
    with (root / f'{mode}-samples.csv').open(newline='') as stream:
        rows = list(csv.DictReader(stream))
    assert rows and all(float(a['elapsed_s']) <= float(b['elapsed_s']) for a, b in zip(rows, rows[1:]))
    for phase in result['phases']:
        name = phase['phase']
        selected = [r for r in rows if r['phase'] == name]
        assert phase['passed'] and phase['errors'] == 0 and phase['unpublished'] == 0, name
        assert phase['work_seconds'] >= minimum_seconds, (name, 'duration too short')
        assert phase['writes'] == phase['checksum'] == phase['expected_checksum'] > 0
        assert phase['created'] == phase['destroyed']
        assert phase['retired'] == phase['reclaimed']
        assert phase['reclaimed'] + phase['teardown_destroyed'] == phase['destroyed']
        assert phase['teardown_destroyed'] == phase['retained_before_teardown']
        assert selected[0]['event'] == 'phase_start' and selected[-1]['event'] == 'phase_end'
        assert all(int(r['errors']) == 0 and r['check_ok'] == '1' for r in selected)
        assert all(int(r['rss_kib']) > 0 and int(r['vms_kib']) >= int(r['rss_kib']) for r in selected)
        for row in selected:
            if row['quiescent'] == '1':
                assert row['checksum'] == row['expected_checksum']
                assert int(row['pending']) == 0 and int(row['retired']) == int(row['reclaimed'])
                assert int(row['live']) == int(row['retained'])
        for field in ('created', 'destroyed', 'transactions', 'writes', 'retired', 'reclaimed',
                      'teardown_destroyed', 'batches', 'threads_started'):
            assert int(selected[-1][field]) == phase[field], (name, field)
        if name == 'slow_reader':
            count = phase['slow_cycles']
            assert count > 0 and count == phase['protected_checks'] == phase['release_checks']
            for event in ('slow_pinned', 'slow_held', 'slow_released', 'slow_drained'):
                assert sum(r['event'] == event for r in selected) == count, event
        if name == 'thread_churn':
            assert phase['batches'] >= 2
            assert sum(r['event'] == 'batch_joined' for r in selected) == phase['batches']
            assert phase['threads_started'] == result['threads'] * phase['batches']
        assert int(selected[-1]['live']) == 0
    return dict(allocator=mode, samples=len(rows),
                active_seconds=sum(p['work_seconds'] for p in result['phases']),
                writes=sum(p['writes'] for p in result['phases']),
                payloads_destroyed=sum(p['destroyed'] for p in result['phases']),
                phases=len(result['phases']))


def verify(root: Path, minimum_seconds: int) -> dict:
    hashes = json.loads((root / 'sha256.json').read_text())
    for name, sha in hashes.items():
        assert hashlib.sha256((root / name).read_bytes()).hexdigest() == sha, name
    state = json.loads((root / 'state.json').read_text())
    assert state['active'] is None and len(state['completed']) == 2
    assert all(run['returncode'] == 0 for run in state['completed'])
    return dict(results=[verify_run(root, mode, minimum_seconds) for mode in ('native', 'system')])


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--min-phase-seconds', type=int, default=600)
    args = parser.parse_args()
    print(json.dumps(verify(args.directory, args.min_phase_seconds), indent=2))
