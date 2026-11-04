#!/usr/bin/env python3
"""Run the reproducible STM benchmark matrix and write CSV artifacts."""

from __future__ import annotations

import argparse
import csv
import hashlib
import io
import json
import os
import platform
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import Dict, Iterable, List, Sequence

SCRIPT_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(SCRIPT_DIR))
from summarize_stm_benchmark import summarize_file  # noqa: E402


RAW_FIELDS = [
    "backend",
    "requested_threads",
    "threads",
    "read_ratio",
    "contention",
    "run",
    "throughput_tps",
    "abort_rate",
    "p50_us",
    "p99_us",
    "attempts",
    "committed",
    "read_checksum",
    "final_checksum",
    "expected_checksum",
    "elapsed_s",
    "valid",
    "error", "failure_reason", "failure_phase", "failure_worker",
    "aborts", "failed_logical", "retries_p50_upper", "retries_p99_upper", "max_retries",
    "warmup_attempts", "warmup_aborts", "warmup_committed", "warmup_failed_logical",
    "abort_snapshot_unavailable", "abort_read_locked", "abort_read_changed",
    "abort_ww_inactive", "abort_ww_commit", "abort_exception",
    "process_returncode", "diagnostics_file", "backoff",
]
BACKENDS = ("mutex", "shared_mutex", "occ", "ww")
FULL_READ_RATIOS = (90, 50, 10)
SMOKE_WORKLOADS = ((90, "low"), (90, "high"), (50, "high"))
FULL_WORKLOADS = tuple(
    (read_ratio, contention)
    for read_ratio in FULL_READ_RATIOS
    for contention in ("low", "high")
)


def run_command(command: Sequence[str], cwd: Path | None = None) -> str:
    completed = subprocess.run(
        list(command),
        cwd=str(cwd) if cwd else None,
        check=False,
        capture_output=True,
        text=True,
    )
    return completed.stdout.strip()


def cpu_model() -> str:
    try:
        with Path("/proc/cpuinfo").open("r", encoding="utf-8") as stream:
            for line in stream:
                if line.lower().startswith("model name") and ":" in line:
                    return line.split(":", 1)[1].strip()
    except OSError:
        pass
    return platform.processor() or "unknown"


def parse_int_list(value: str) -> List[int]:
    result = []
    for item in value.split(","):
        item = item.strip()
        if not item:
            continue
        parsed = int(item)
        if parsed <= 0:
            raise ValueError("thread counts and repetitions must be positive")
        result.append(parsed)
    if not result:
        raise ValueError("list must not be empty")
    return result


def default_thread_points(smoke: bool, logical_cpus: int) -> List[int]:
    requested = (1, 4, 8) if smoke else (1, 2, 4, 8, 16, 32)
    points: List[int] = []
    for value in requested:
        actual = min(value, logical_cpus)
        if actual not in points:
            points.append(actual)
    if not smoke and logical_cpus > 16 and logical_cpus not in points:
        points.append(logical_cpus)
    return points


def parse_binary_row(stdout: str) -> Dict[str, str] | None:
    try:
        rows = list(csv.DictReader(io.StringIO(stdout), strict=True))
    except csv.Error:
        return None
    if len(rows) != 1:
        return None
    row = rows[0]
    required = {"backend", "threads", "attempts", "committed", "valid"}
    if None in row or not required.issubset(row) or any(value is None for value in row.values()):
        return None
    if row["valid"] not in {"0", "1"}:
        return None
    return row


def failed_row(backend: str, threads: int, read_ratio: int,
               contention: str, run: int, error: str,
               reason: str = "process_exit") -> Dict[str, str]:
    return {
        "backend": backend,
        "requested_threads": str(threads),
        "threads": "",
        "read_ratio": str(read_ratio),
        "contention": contention,
        "run": str(run),
        "throughput_tps": "",
        "abort_rate": "",
        "p50_us": "",
        "p99_us": "",
        "attempts": "",
        "committed": "",
        "read_checksum": "",
        "final_checksum": "",
        "expected_checksum": "",
        "elapsed_s": "",
        "valid": "0",
        "error": error,
        "failure_reason": reason,
        "failure_phase": "process",
    }


def normalize_result(completed: subprocess.CompletedProcess, backend: str, threads: int,
                     read_ratio: int, contention: str, run: int) -> Dict[str, str]:
    parsed = parse_binary_row(completed.stdout)
    process_reason = ("signal" if completed.returncode < 0 else
                      "process_exit" if completed.returncode else "parse_error")
    if parsed is None:
        row = failed_row(backend, threads, read_ratio, contention, run,
                         completed.stderr.strip() or f"exit code {completed.returncode}; no valid CSV row",
                         process_reason)
    else:
        row = {field: parsed.get(field, "") for field in RAW_FIELDS}
        row["error"] = " | ".join(filter(None, (row["error"], completed.stderr.strip())))
        if completed.returncode:
            row["valid"] = "0"
            if row["failure_reason"] in {"", "none"}:
                row["failure_reason"] = process_reason
                row["failure_phase"] = "process"
            if not row["error"]:
                row["error"] = f"exit code {completed.returncode}"
        elif row["valid"] == "0" and row["failure_reason"] in {"", "none"}:
            row["failure_reason"] = "unspecified_invalid"
    row["process_returncode"] = str(completed.returncode)
    return row


def source_manifest(repo: Path) -> Dict[str, str]:
    paths = subprocess.check_output(
        ["git", "ls-files", "-z", "--cached", "--others", "--exclude-standard"], cwd=repo
    ).decode().split("\0")
    return {name: hashlib.sha256((repo / name).read_bytes()).hexdigest()
            for name in sorted(set(paths)) if name and (repo / name).is_file()
            and (name == "CMakeLists.txt" or name.startswith(
                ("include/", "src/", "benchmarks/", "tests/", "scripts/")))
            and (repo / name).suffix in {".hpp", ".cpp", ".py", ".txt", ".cmake"}}


def metadata_text(repo: Path, binary: Path, args: argparse.Namespace,
                  threads: Sequence[int], workloads: Iterable[Sequence[object]]) -> str:
    commit = run_command(("git", "-C", str(repo), "rev-parse", "HEAD"))
    compiler = run_command(("c++", "--version")).splitlines()
    parameters = {
        "backends": args.backends,
        "backoff": args.backoff,
        "threads": list(threads),
        "workloads": [list(item) for item in workloads],
        "repetitions": args.repetitions,
        "warmup_ms": args.warmup_ms,
        "measure_ms": args.measure_ms,
        "ops_per_tx": args.ops_per_tx,
        "low_key_count": args.low_key_count,
        "high_key_count": args.high_key_count,
        "seed": args.seed,
        "max_attempts": args.max_attempts,
        "latency_sampling": "one logical transaction per 256 committed transactions",
    }
    lines = [
        "benchmark=CaSTM unified STM benchmark",
        "binary_sha256=" + hashlib.sha256(binary.read_bytes()).hexdigest(),
        "working_tree_dirty=" + str(bool(run_command(("git", "status", "--porcelain"), repo))).lower(),
        "source_manifest=" + json.dumps(source_manifest(repo), sort_keys=True),
        f"git_commit={commit or 'unknown'}",
        f"binary={binary}",
        "build_type=Release",
        "optimization=-O3",
        "NDEBUG=1",
        "STM_WW_VERIFY_LOGIC_MODE=0",
        "STM_WW_TEST_HOOKS=0",
        "march_native=opt-in (disabled unless the build requested STM_BENCHMARK_NATIVE)",
        f"compiler={(compiler[0] if compiler else 'unknown')}",
        f"cpu_model={cpu_model()}",
        f"logical_cpu_count={os.cpu_count() or 1}",
        f"os={platform.platform()}",
        f"kernel={platform.release()}",
        "contention_low=65536 keys unless --low-key-count overrides it",
        "contention_high=16 keys unless --high-key-count overrides it",
        "measurement_window=worker starts; in-flight logical transactions finish before join",
        "parameters=" + json.dumps(parameters, sort_keys=True),
    ]
    return "\n".join(lines) + "\n"


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True,
                        help="Release benchmarks/stm_benchmark executable")
    parser.add_argument("--output-dir", type=Path,
                        help="exact result directory; defaults to bench/results/<UTC timestamp>")
    parser.add_argument("--smoke", action="store_true",
                        help="use 1/4/8 threads and three short representative workloads")
    parser.add_argument("--backends", default=",".join(BACKENDS),
                        help="comma-separated backend list")
    parser.add_argument("--threads", help="comma-separated thread counts")
    parser.add_argument("--repetitions", type=int, default=3)
    parser.add_argument("--warmup-ms", type=int, default=1000)
    parser.add_argument("--measure-ms", type=int, default=5000)
    parser.add_argument("--ops-per-tx", type=int, default=8)
    parser.add_argument("--low-key-count", type=int, default=65536)
    parser.add_argument("--high-key-count", type=int, default=16)
    parser.add_argument("--seed", type=lambda value: int(value, 0), default=0xCA57BEEF)
    parser.add_argument("--backoff", choices=("none", "yield", "jitter"), default="none")
    parser.add_argument("--max-attempts", type=int, default=1000000)
    parser.add_argument("--timeout-seconds", type=float,
                        help="per process timeout; default is measure budget plus 30 seconds")
    return parser


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()
    if args.repetitions <= 0 or args.warmup_ms < 0 or args.measure_ms <= 0:
        parser.error("repetitions must be positive, warmup-ms non-negative, measure-ms positive")
    if args.ops_per_tx <= 0 or args.low_key_count <= 0 or args.high_key_count <= 0:
        parser.error("operation count and key counts must be positive")
    if args.max_attempts <= 0:
        parser.error("max-attempts must be positive")

    try:
        backends = [item.strip() for item in args.backends.split(",") if item.strip()]
        if not backends or any(item not in BACKENDS for item in backends):
            raise ValueError(f"backends must be drawn from {BACKENDS}")
        if args.backoff != "none" and backends != ["occ"]:
            raise ValueError("non-default backoff requires --backends occ")
        logical_cpus = max(1, os.cpu_count() or 1)
        threads = (parse_int_list(args.threads)
                   if args.threads else default_thread_points(args.smoke, logical_cpus))
    except ValueError as error:
        parser.error(str(error))

    workloads = SMOKE_WORKLOADS if args.smoke else FULL_WORKLOADS
    binary = args.binary.resolve()
    if not binary.is_file():
        parser.error(f"benchmark binary does not exist: {binary}")

    if args.output_dir:
        output_dir = args.output_dir.resolve()
    else:
        timestamp = datetime.now(timezone.utc).strftime("%Y%m%d-%H%M%S")
        output_dir = (Path(__file__).resolve().parents[1] /
                      "bench" / "results" / timestamp)
    output_dir.mkdir(parents=True, exist_ok=False)

    repo = Path(__file__).resolve().parents[1]
    raw_path = output_dir / "raw.csv"
    summary_path = output_dir / "summary.csv"
    metadata_path = output_dir / "metadata.txt"
    (output_dir / "diagnostics").mkdir()
    metadata_path.write_text(metadata_text(repo, binary, args, threads, workloads),
                              encoding="utf-8")

    total = len(backends) * len(threads) * len(tuple(workloads)) * args.repetitions
    completed_points = 0
    had_invalid = False
    timeout = args.timeout_seconds or max(30.0, args.warmup_ms / 1000.0 +
                                          args.measure_ms / 1000.0 + 30.0)

    with raw_path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=RAW_FIELDS)
        writer.writeheader()
        for backend in backends:
            for thread_count in threads:
                for read_ratio, contention in workloads:
                    key_count = (args.low_key_count if contention == "low"
                                 else args.high_key_count)
                    for run in range(1, args.repetitions + 1):
                        diagnostic = Path("diagnostics") / f"{backend}-t{thread_count}-r{read_ratio}-{contention}-{run}.json"
                        command = [
                            str(binary),
                            "--backend", backend,
                            "--threads", str(thread_count),
                            "--read-ratio", str(read_ratio),
                            "--contention", contention,
                            "--key-count", str(key_count),
                            "--ops-per-tx", str(args.ops_per_tx),
                            "--warmup-ms", str(args.warmup_ms),
                            "--measure-ms", str(args.measure_ms),
                            "--run", str(run),
                            "--seed", str(args.seed),
                            "--max-attempts", str(args.max_attempts),
                            "--backoff", args.backoff,
                            "--diagnostics-json", str(output_dir / diagnostic),
                            "--csv",
                        ]
                        try:
                            completed = subprocess.run(
                                command,
                                cwd=str(repo),
                                check=False,
                                capture_output=True,
                                text=True,
                                timeout=timeout,
                            )
                            row = normalize_result(completed, backend, thread_count,
                                                   read_ratio, contention, run)
                        except subprocess.TimeoutExpired:
                            row = failed_row(backend, thread_count, read_ratio,
                                             contention, run, f"timeout after {timeout:.1f}s", "timeout")
                        except OSError as error:
                            row = failed_row(backend, thread_count, read_ratio,
                                             contention, run, str(error), "launch_error")
                        row["backoff"] = args.backoff
                        row["diagnostics_file"] = str(diagnostic) if (output_dir / diagnostic).exists() else ""
                        writer.writerow(row)
                        stream.flush()
                        if row.get("valid", "0") not in {"1", "true", "yes"}:
                            had_invalid = True
                        completed_points += 1
                        print(f"[{completed_points}/{total}] {backend} "
                              f"threads={thread_count} read={read_ratio} "
                              f"contention={contention} run={run} "
                              f"valid={row.get('valid', '0')}", file=sys.stderr)

    summary_file = summarize_file(raw_path, summary_path)
    valid_count = sum(int(row["valid_runs"]) for row in summary_file)
    print(f"raw={raw_path}", file=sys.stderr)
    print(f"summary={summary_path}", file=sys.stderr)
    print(f"valid_runs={valid_count}", file=sys.stderr)
    return 1 if had_invalid else 0


if __name__ == "__main__":
    raise SystemExit(main())
