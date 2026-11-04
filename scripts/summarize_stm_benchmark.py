#!/usr/bin/env python3
"""Summarize raw stm_benchmark CSV output without external dependencies."""

from __future__ import annotations

import argparse
import csv
import statistics
from collections import defaultdict
from pathlib import Path
from typing import Dict, Iterable, List, Sequence, Tuple


GROUP_FIELDS = ("backend", "threads", "read_ratio", "contention")
METRICS = (
    ("throughput_tps", "throughput_mean_tps", "throughput_stddev_tps"),
    ("abort_rate", "abort_rate_mean", "abort_rate_stddev"),
    ("p50_us", "p50_mean_us", "p50_stddev_us"),
    ("p99_us", "p99_mean_us", "p99_stddev_us"),
)


def _is_valid(row: Dict[str, str]) -> bool:
    return row.get("valid", "0").strip().lower() in {"1", "true", "yes"}


def _mean_stddev(values: Sequence[float]) -> Tuple[float, float]:
    if not values:
        return 0.0, 0.0
    if len(values) == 1:
        return values[0], 0.0
    return statistics.mean(values), statistics.stdev(values)


def summarize_rows(rows: Iterable[Dict[str, str]]) -> List[Dict[str, str]]:
    grouped = defaultdict(list)
    all_rows = list(rows)
    group_fields = GROUP_FIELDS + (("backoff",) if any("backoff" in row for row in all_rows) else ())
    for row in all_rows:
        grouped[tuple(row.get(field, "none" if field == "backoff" else "") for field in group_fields)].append(row)

    output: List[Dict[str, str]] = []
    for key in sorted(grouped):
        group = grouped[key]
        valid_rows = [row for row in group if _is_valid(row)]
        result = {field: value for field, value in zip(group_fields, key)}
        result["valid_runs"] = str(len(valid_rows))
        result["total_runs"] = str(len(group))

        for source, mean_name, stddev_name in METRICS:
            values = []
            for row in valid_rows:
                try:
                    values.append(float(row[source]))
                except (KeyError, TypeError, ValueError):
                    pass
            mean, stddev = _mean_stddev(values)
            result[mean_name] = f"{mean:.6f}" if values else ""
            result[stddev_name] = f"{stddev:.6f}" if values else ""
        output.append(result)
    return output


def summarize_file(raw_path: Path, summary_path: Path) -> List[Dict[str, str]]:
    with raw_path.open("r", newline="", encoding="utf-8") as stream:
        rows = list(csv.DictReader(stream))

    summary = summarize_rows(rows)
    fieldnames = list(GROUP_FIELDS) + (["backoff"] if any("backoff" in row for row in rows) else []) + ["valid_runs", "total_runs"]
    for _, mean_name, stddev_name in METRICS:
        fieldnames.extend((mean_name, stddev_name))

    with summary_path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(summary)
    return summary


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("raw_csv", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    summarize_file(args.raw_csv, args.output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
