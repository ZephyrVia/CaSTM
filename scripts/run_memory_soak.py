#!/usr/bin/env python3
"""Run the native and system allocator lifetime validations sequentially."""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import platform
import subprocess
import sys
import time

from run_stm_benchmark import cpu_model, source_manifest


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native-binary', type=Path, required=True)
    parser.add_argument('--system-binary', type=Path, required=True)
    parser.add_argument('--output-dir', type=Path, required=True)
    defaults = dict(phase_seconds=600, threads=20, sample_ms=1000,
                    rate_per_worker=1000, hold_ms=5000, recovery_ms=5000,
                    batch_ms=5000, batch_transactions=2000, settle_ms=2000,
                    rss_limit_mib=1024, vms_limit_mib=8192)
    for name, default in defaults.items():
        parser.add_argument('--' + name.replace('_', '-'), type=int, default=default)
    args = parser.parse_args()
    if any(getattr(args, name) <= 0 for name in defaults):
        parser.error('all numeric options must be positive')
    binaries = dict(native=args.native_binary.resolve(), system=args.system_binary.resolve())
    if not all(p.is_file() for p in binaries.values()):
        parser.error('both binaries must exist')
    root = args.output_dir.resolve()
    if root.exists():
        parser.error('output directory must not exist')
    root.mkdir(parents=True)
    repo = Path(__file__).resolve().parents[1]
    git = lambda *parts: subprocess.check_output(['git', *parts], cwd=repo, text=True).strip()
    metadata = dict(schema_version=1, started_utc=datetime.now(timezone.utc).isoformat(),
                    source_commit=git('rev-parse', 'HEAD'), branch=git('branch', '--show-current'),
                    working_tree_dirty=bool(git('status', '--porcelain')),
                    source_manifest=source_manifest(repo), cpu=cpu_model(),
                    platform=platform.platform(), python=sys.version,
                    parameters={name: getattr(args, name) for name in defaults},
                    binaries={name: dict(path=str(p), sha256=digest(p)) for name, p in binaries.items()},
                    measurement='Paced lifetime validation; payload counters, not production EBR hooks.',
                    run_order=['native', 'system'])
    (root / 'metadata.json').write_text(json.dumps(metadata, indent=2) + '\n')
    state = dict(completed=[], active=None)
    invalid = False
    for mode, binary in binaries.items():
        command = [str(binary), '--samples', str(root / f'{mode}-samples.csv'),
                   '--result', str(root / f'{mode}-result.json')]
        for name in defaults:
            command += ['--' + name.replace('_', '-'), str(getattr(args, name))]
        state['active'] = dict(allocator=mode, started_utc=datetime.now(timezone.utc).isoformat(),
                               command=command)
        (root / 'state.json').write_text(json.dumps(state, indent=2) + '\n')
        start = time.monotonic()
        record = dict(allocator=mode, command=command)
        print(f'start {mode}: {args.phase_seconds * 3}s of active validation', flush=True)
        with (root / f'{mode}-process.txt').open('w') as stream:
            try:
                done = subprocess.run(command, cwd=repo, stdout=stream, stderr=subprocess.STDOUT,
                                      timeout=3 * args.phase_seconds + 180, check=False)
                record['returncode'] = done.returncode
            except subprocess.TimeoutExpired:
                record.update(returncode=None, failure='timeout')
        record['wall_seconds'] = time.monotonic() - start
        invalid |= record['returncode'] != 0
        state['completed'].append(record)
        state['active'] = None
        (root / 'state.json').write_text(json.dumps(state, indent=2) + '\n')
        print(f'finish {mode}: returncode={record["returncode"]}', flush=True)
    state['finished_utc'] = datetime.now(timezone.utc).isoformat()
    (root / 'state.json').write_text(json.dumps(state, indent=2) + '\n')
    hashes = {p.name: digest(p) for p in root.iterdir() if p.is_file()}
    (root / 'sha256.json').write_text(json.dumps(hashes, indent=2, sort_keys=True) + '\n')
    return int(invalid)


if __name__ == '__main__':
    raise SystemExit(main())
