#!/usr/bin/env python3
"""Compare OccSTM retry policies in rotated order using one benchmark binary."""
from __future__ import annotations

import csv
import json
from pathlib import Path
import subprocess
import sys

from run_stm_benchmark import (
    RAW_FIELDS, FULL_WORKLOADS, build_parser, parse_int_list, metadata_text,
    normalize_result, failed_row,
)
from summarize_stm_benchmark import summarize_file


def main() -> int:
    parser = build_parser()
    parser.description = __doc__
    parser.add_argument('--policies', default='none,yield,jitter')
    args = parser.parse_args()
    policies = args.policies.split(',')
    if (not policies or len(set(policies)) != len(policies)
            or any(p not in {'none', 'yield', 'jitter'} for p in policies)):
        parser.error('policies must be distinct choices from none,yield,jitter')
    if not args.output_dir:
        parser.error('--output-dir is required')
    if args.repetitions <= 0 or args.warmup_ms < 0 or args.measure_ms <= 0 or args.max_attempts <= 0:
        parser.error('invalid repetition, duration or attempt budget')
    if args.ops_per_tx <= 0 or args.ops_per_tx > 64 or min(args.low_key_count, args.high_key_count) <= 0:
        parser.error('invalid operation count or key count')
    if args.smoke or args.backoff != 'none':
        parser.error('use --policies; --smoke and --backoff are not used by this ablation')
    args.backends = 'occ'
    threads = parse_int_list(args.threads or '1,20')
    binary = args.binary.resolve()
    if not binary.is_file():
        parser.error('binary does not exist')
    output = args.output_dir.resolve()
    (output / 'diagnostics').mkdir(parents=True, exist_ok=False)
    repo = Path(__file__).resolve().parents[1]
    (output / 'metadata.txt').write_text(
        metadata_text(repo, binary, args, threads, FULL_WORKLOADS)
        + 'policies=' + json.dumps(policies) + '\n'
        + 'execution_order=per workload and repetition, rotate policy order by run-1\n', encoding='utf-8')
    timeout = args.timeout_seconds or max(30.0, (args.warmup_ms + args.measure_ms) / 1000 + 30)
    invalid = 0
    index = 0
    with (output / 'raw.csv').open('w', newline='', encoding='utf-8') as stream:
        writer = csv.DictWriter(stream, fieldnames=RAW_FIELDS)
        writer.writeheader()
        for thread_count in threads:
            for read_ratio, contention in FULL_WORKLOADS:
                for run in range(1, args.repetitions + 1):
                    shift = (run - 1) % len(policies)
                    for policy in policies[shift:] + policies[:shift]:
                        diagnostic = Path('diagnostics') / f't{thread_count}-r{read_ratio}-{contention}-{run}-{policy}.json'
                        command = [str(binary), '--backend', 'occ', '--backoff', policy,
                                   '--threads', str(thread_count), '--read-ratio', str(read_ratio),
                                   '--contention', contention, '--key-count',
                                   str(args.low_key_count if contention == 'low' else args.high_key_count),
                                   '--ops-per-tx', str(args.ops_per_tx), '--warmup-ms', str(args.warmup_ms),
                                   '--measure-ms', str(args.measure_ms), '--run', str(run),
                                   '--seed', str(args.seed), '--max-attempts', str(args.max_attempts),
                                   '--diagnostics-json', str(output / diagnostic), '--csv']
                        try:
                            completed = subprocess.run(command, cwd=repo, capture_output=True,
                                                       text=True, timeout=timeout, check=False)
                            row = normalize_result(completed, 'occ', thread_count, read_ratio, contention, run)
                        except subprocess.TimeoutExpired:
                            row = failed_row('occ', thread_count, read_ratio, contention, run,
                                             f'timeout after {timeout:.1f}s', 'timeout')
                        except OSError as error:
                            row = failed_row('occ', thread_count, read_ratio, contention, run,
                                             str(error), 'launch_error')
                        row['backoff'] = policy
                        row['diagnostics_file'] = str(diagnostic) if (output / diagnostic).exists() else ''
                        writer.writerow(row)
                        stream.flush()
                        index += 1
                        invalid += row['valid'] != '1'
                        print(f'[{index}] threads={thread_count} read={read_ratio} {contention} '
                              f'run={run} {policy} valid={row["valid"]} '
                              f'reason={row.get("failure_reason", "")}', file=sys.stderr)
    summarize_file(output / 'raw.csv', output / 'summary.csv')
    print(f'runs={index} invalid={invalid} output={output}', file=sys.stderr)
    return int(invalid > 0)


if __name__ == '__main__':
    raise SystemExit(main())
