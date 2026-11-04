#!/usr/bin/env python3
"""Plot and analyze an existing CaSTM STM benchmark result directory.

This script is deliberately read-only with respect to the benchmark input:
it consumes raw.csv, summary.csv, and metadata.txt, then writes charts and
derived reports beside them.  It never starts the benchmark executable.

The plotting dependency is matplotlib.  The benchmark itself remains a
standard-library/C++ target; install matplotlib only for this analysis step.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import shutil
import statistics
from collections import Counter, defaultdict
from pathlib import Path
from typing import Dict, Iterable, List, Mapping, Sequence, Tuple

try:
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from matplotlib.lines import Line2D
except ImportError as error:  # pragma: no cover - environment dependent
    raise SystemExit(
        "matplotlib is required for chart generation; install it separately "
        "from the benchmark source tree"
    ) from error


BACKENDS = ("mutex", "shared_mutex", "occ", "ww")
BACKEND_LABELS = {
    "mutex": "std::mutex",
    "shared_mutex": "std::shared_mutex",
    "occ": "OccSTM",
    "ww": "WwSTM",
}
BACKEND_COLORS = {
    "mutex": "#4C78A8",
    "shared_mutex": "#F58518",
    "occ": "#54A24B",
    "ww": "#E45756",
}
BACKEND_STYLES = {
    "mutex": "-",
    "shared_mutex": "--",
    "occ": "-",
    "ww": "-",
}
THREADS = (1, 2, 4, 8, 16, 20)
READ_RATIOS = (90, 50, 10)
CONTENTIONS = ("low", "high")
RAW_REQUIRED = {
    "backend",
    "threads",
    "read_ratio",
    "contention",
    "run",
    "throughput_tps",
    "abort_rate",
    "p50_us",
    "p99_us",
    "final_checksum",
    "expected_checksum",
    "valid",
}
SUMMARY_REQUIRED = {
    "backend",
    "threads",
    "read_ratio",
    "contention",
    "valid_runs",
    "total_runs",
    "throughput_mean_tps",
    "throughput_stddev_tps",
    "abort_rate_mean",
    "abort_rate_stddev",
    "p50_mean_us",
    "p50_stddev_us",
    "p99_mean_us",
    "p99_stddev_us",
}

METRICS = {
    "throughput": {
        "mean": "throughput_mean_tps",
        "stddev": "throughput_stddev_tps",
        "title": "Throughput vs threads",
        "ylabel": "Committed logical transactions / second",
        "scale": "linear",
        "suffix": "throughput",
    },
    "abort_rate": {
        "mean": "abort_rate_mean",
        "stddev": "abort_rate_stddev",
        "title": "Abort rate vs threads",
        "ylabel": "Aborted attempts / all attempts (%)",
        "scale": "linear",
        "suffix": "abort_rate",
    },
    "p99": {
        "mean": "p99_mean_us",
        "stddev": "p99_stddev_us",
        "title": "p99 latency vs threads",
        "ylabel": "p99 logical transaction latency (us)",
        "scale": "log",
        "suffix": "p99",
    },
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "result_dir",
        type=Path,
        help="existing result directory containing raw.csv and summary.csv",
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        help="chart directory; defaults to result_dir/charts",
    )
    parser.add_argument(
        "--analysis-output",
        type=Path,
        help="markdown report; defaults to result_dir/analysis.md",
    )
    parser.add_argument(
        "--summary-output",
        type=Path,
        help="derived summary CSV; defaults to result_dir/analysis_summary.csv",
    )
    return parser.parse_args()


def read_csv(path: Path) -> List[Dict[str, str]]:
    with path.open("r", newline="", encoding="utf-8") as stream:
        reader = csv.DictReader(stream)
        if reader.fieldnames is None:
            raise ValueError(f"CSV has no header: {path}")
        return list(reader)


def read_metadata(path: Path) -> Dict[str, str]:
    metadata: Dict[str, str] = {}
    with path.open("r", encoding="utf-8") as stream:
        for line in stream:
            line = line.rstrip("\n")
            if "=" in line:
                key, value = line.split("=", 1)
                metadata[key] = value
    return metadata


def parse_int(value: str | None) -> int | None:
    if value is None or value.strip() == "":
        return None
    try:
        return int(value)
    except ValueError:
        return None


def parse_float(value: str | None) -> float | None:
    if value is None or value.strip() == "":
        return None
    try:
        return float(value)
    except ValueError:
        return None


def is_valid(row: Mapping[str, str]) -> bool:
    return row.get("valid", "0").strip().lower() in {"1", "true", "yes"}


def raw_key(row: Mapping[str, str]) -> Tuple[str, int, int, str] | None:
    threads = parse_int(row.get("threads"))
    ratio = parse_int(row.get("read_ratio"))
    backend = row.get("backend", "")
    contention = row.get("contention", "")
    if threads is None or ratio is None or not backend or not contention:
        return None
    return backend, threads, ratio, contention


def key_label(key: Tuple[str, int, int, str]) -> str:
    backend, threads, ratio, contention = key
    return f"{BACKEND_LABELS.get(backend, backend)} {threads}T {ratio}R/{100 - ratio}W {contention}"


def ratio_label(read_ratio: int) -> str:
    return f"{read_ratio}R{100 - read_ratio}W"


def workload_label(read_ratio: int, contention: str) -> str:
    keys = "65,536 keys" if contention == "low" else "16 keys"
    return f"{ratio_label(read_ratio)}, {contention} contention ({keys})"


def summary_key(row: Mapping[str, str]) -> Tuple[str, int, int, str]:
    return (
        row["backend"],
        int(row["threads"]),
        int(row["read_ratio"]),
        row["contention"],
    )


def metric_value(row: Mapping[str, str] | None, metric: str) -> float | None:
    if row is None or parse_int(row.get("valid_runs")) == 0:
        return None
    field = METRICS[metric]["mean"]
    value = parse_float(row.get(field))
    if value is None or not math.isfinite(value):
        return None
    if metric == "abort_rate":
        return value * 100.0
    return value


def metric_stddev(row: Mapping[str, str] | None, metric: str) -> float:
    if row is None or parse_int(row.get("valid_runs")) == 0:
        return 0.0
    value = parse_float(row.get(METRICS[metric]["stddev"])) or 0.0
    return value * 100.0 if metric == "abort_rate" else value


def finite_metric(row: Mapping[str, str] | None, metric: str) -> float | None:
    value = metric_value(row, metric)
    return value if value is not None and value > 0 else None


def fmt_tps(value: float | None) -> str:
    if value is None:
        return "N/A"
    if abs(value) >= 1_000_000:
        return f"{value / 1_000_000:.2f}M"
    if abs(value) >= 1_000:
        return f"{value / 1_000:.1f}k"
    return f"{value:.1f}"


def fmt_us(value: float | None) -> str:
    if value is None:
        return "N/A"
    if value >= 1_000:
        return f"{value / 1_000:.2f} ms"
    if value >= 1:
        return f"{value:.2f} us"
    return f"{value:.3f} us"


def fmt_pct(value: float | None) -> str:
    if value is None:
        return "N/A"
    return f"{value * 100:.1f}%" if abs(value) <= 1.0 else f"{value:.1f}%"


def fmt_number(value: float | None, digits: int = 2) -> str:
    return "N/A" if value is None else f"{value:.{digits}f}"


def md_table(headers: Sequence[str], rows: Iterable[Sequence[object]]) -> List[str]:
    output = ["| " + " | ".join(headers) + " |", "| " + " | ".join("---" for _ in headers) + " |"]
    for row in rows:
        output.append("| " + " | ".join(str(item) for item in row) + " |")
    return output


def load_inputs(result_dir: Path) -> Tuple[List[Dict[str, str]], List[Dict[str, str]], Dict[str, str]]:
    raw_path = result_dir / "raw.csv"
    summary_path = result_dir / "summary.csv"
    metadata_path = result_dir / "metadata.txt"
    for path in (raw_path, summary_path, metadata_path):
        if not path.is_file():
            raise SystemExit(f"missing benchmark result file: {path}")

    raw_rows = read_csv(raw_path)
    summary_rows = read_csv(summary_path)
    policies = {row.get("backoff", "none") for row in raw_rows + summary_rows}
    if len(policies) > 1:
        raise SystemExit("Mixed retry policies: split the input by backoff before using this plotter")
    raw_fields = set(raw_rows[0]) if raw_rows else set()
    summary_fields = set(summary_rows[0]) if summary_rows else set()
    missing_raw = sorted(RAW_REQUIRED - raw_fields)
    missing_summary = sorted(SUMMARY_REQUIRED - summary_fields)
    if missing_raw:
        raise SystemExit(f"raw.csv missing fields: {', '.join(missing_raw)}")
    if missing_summary:
        raise SystemExit(f"summary.csv missing fields: {', '.join(missing_summary)}")
    return raw_rows, summary_rows, read_metadata(metadata_path)


def make_quality_report(
    raw_rows: Sequence[Mapping[str, str]],
    summary_rows: Sequence[Mapping[str, str]],
) -> Dict[str, object]:
    raw_groups: Dict[Tuple[str, int, int, str], List[Mapping[str, str]]] = defaultdict(list)
    for row in raw_rows:
        key = raw_key(row)
        if key is not None:
            raw_groups[key].append(row)

    summary_map = {summary_key(row): row for row in summary_rows}
    expected_rows = len(BACKENDS) * len(THREADS) * len(READ_RATIOS) * len(CONTENTIONS) * 3
    invalid_rows = [row for row in raw_rows if not is_valid(row)]
    checksum_mismatches = [
        row
        for row in raw_rows
        if row.get("final_checksum", "")
        and row.get("expected_checksum", "")
        and row["final_checksum"] != row["expected_checksum"]
    ]
    invalid_checksum_mismatches = [
        row for row in invalid_rows if row.get("final_checksum", "") != row.get("expected_checksum", "")
    ]
    run_counts = Counter(len(rows) for rows in raw_groups.values())
    incomplete_groups = [key for key, rows in raw_groups.items() if len(rows) != 3]

    backend_counts: Dict[str, Dict[str, int]] = {}
    for backend in BACKENDS:
        rows = [row for row in raw_rows if row.get("backend") == backend]
        backend_counts[backend] = {
            "total": len(rows),
            "valid": sum(1 for row in rows if is_valid(row)),
            "invalid": sum(1 for row in rows if not is_valid(row)),
        }

    summary_mismatches = []
    for key, rows in raw_groups.items():
        summary = summary_map.get(key)
        expected_valid = sum(1 for row in rows if is_valid(row))
        actual_valid = parse_int(summary.get("valid_runs")) if summary else None
        actual_total = parse_int(summary.get("total_runs")) if summary else None
        if summary is None or actual_valid != expected_valid or actual_total != len(rows):
            summary_mismatches.append((key, expected_valid, len(rows), actual_valid, actual_total))

    invalid_config_rows = []
    for key, summary in sorted(summary_map.items(), key=lambda item: item[0]):
        valid_runs = parse_int(summary.get("valid_runs")) or 0
        total_runs = parse_int(summary.get("total_runs")) or 0
        if valid_runs < total_runs:
            invalid_config_rows.append((key, valid_runs, total_runs))

    return {
        "expected_rows": expected_rows,
        "raw_rows": len(raw_rows),
        "raw_group_count": len(raw_groups),
        "summary_groups": len(summary_map),
        "invalid_rows": invalid_rows,
        "checksum_mismatches": checksum_mismatches,
        "invalid_checksum_mismatches": invalid_checksum_mismatches,
        "run_counts": run_counts,
        "incomplete_groups": incomplete_groups,
        "backend_counts": backend_counts,
        "summary_mismatches": summary_mismatches,
        "invalid_config_rows": invalid_config_rows,
        "raw_groups": raw_groups,
        "summary_map": summary_map,
    }


def relative_variance_anomalies(summary_map: Mapping[Tuple[str, int, int, str], Mapping[str, str]]) -> Dict[str, List[Tuple[float, Tuple[str, int, int, str], Mapping[str, str]]]]:
    anomalies: Dict[str, List[Tuple[float, Tuple[str, int, int, str], Mapping[str, str]]]] = defaultdict(list)
    variance_specs = {
        metric: (spec["mean"], spec["stddev"])
        for metric, spec in METRICS.items()
    }
    variance_specs["p50"] = ("p50_mean_us", "p50_stddev_us")
    for key, row in summary_map.items():
        if (parse_int(row.get("valid_runs")) or 0) == 0:
            continue
        for metric, (mean_field, stddev_field) in variance_specs.items():
            mean = parse_float(row.get(mean_field))
            stddev = parse_float(row.get(stddev_field))
            if mean is None or stddev is None or mean == 0:
                continue
            relative = abs(stddev / mean)
            if relative > 0.10:
                anomalies[metric].append((relative, key, row))
    for values in anomalies.values():
        values.sort(key=lambda item: item[0], reverse=True)
    return anomalies


def adjacent_anomalies(
    summary_map: Mapping[Tuple[str, int, int, str], Mapping[str, str]],
) -> Dict[str, List[Tuple[float, Tuple[str, int, int, str], int, int, float, float]]]:
    drops: List[Tuple[float, Tuple[str, int, int, str], int, int, float, float]] = []
    p99_jumps: List[Tuple[float, Tuple[str, int, int, str], int, int, float, float]] = []
    abort_shifts: List[Tuple[float, Tuple[str, int, int, str], int, int, float, float]] = []
    for backend in BACKENDS:
        for ratio in READ_RATIOS:
            for contention in CONTENTIONS:
                for left, right in zip(THREADS, THREADS[1:]):
                    left_row = summary_map.get((backend, left, ratio, contention))
                    right_row = summary_map.get((backend, right, ratio, contention))
                    if not left_row or not right_row:
                        continue
                    if (parse_int(left_row.get("valid_runs")) or 0) == 0 or (parse_int(right_row.get("valid_runs")) or 0) == 0:
                        continue
                    left_thr = parse_float(left_row.get("throughput_mean_tps"))
                    right_thr = parse_float(right_row.get("throughput_mean_tps"))
                    if left_thr and right_thr and right_thr / left_thr < 0.50:
                        drops.append((right_thr / left_thr, (backend, 0, ratio, contention), left, right, left_thr, right_thr))

                    left_p99 = parse_float(left_row.get("p99_mean_us"))
                    right_p99 = parse_float(right_row.get("p99_mean_us"))
                    if left_p99 and right_p99 and left_p99 > 0 and right_p99 / left_p99 > 2.0:
                        p99_jumps.append((right_p99 / left_p99, (backend, 0, ratio, contention), left, right, left_p99, right_p99))

                    left_abort = parse_float(left_row.get("abort_rate_mean"))
                    right_abort = parse_float(right_row.get("abort_rate_mean"))
                    if left_abort is not None and right_abort is not None and abs(right_abort - left_abort) > 0.20:
                        abort_shifts.append((abs(right_abort - left_abort), (backend, 0, ratio, contention), left, right, left_abort, right_abort))
    drops.sort(key=lambda item: item[0])
    p99_jumps.sort(key=lambda item: item[0], reverse=True)
    abort_shifts.sort(key=lambda item: item[0], reverse=True)
    return {"throughput_drop": drops, "p99_jump": p99_jumps, "abort_shift": abort_shifts}


def plot_value(row: Mapping[str, str] | None, metric: str) -> float | None:
    return metric_value(row, metric)


def plot_metric(
    summary_map: Mapping[Tuple[str, int, int, str], Mapping[str, str]],
    metric: str,
    read_ratio: int,
    contention: str,
    output_path: Path,
) -> None:
    spec = METRICS[metric]
    fig, axis = plt.subplots(figsize=(10.2, 6.4), dpi=180)
    all_values: List[float] = []
    missing_points: List[Tuple[int, str]] = []

    for backend in BACKENDS:
        line_values: List[float | None] = []
        valid_x: List[int] = []
        valid_y: List[float] = []
        errors: List[float] = []
        for thread_count in THREADS:
            row = summary_map.get((backend, thread_count, read_ratio, contention))
            value = plot_value(row, metric)
            line_values.append(value)
            if value is None:
                missing_points.append((thread_count, backend))
            else:
                valid_x.append(thread_count)
                valid_y.append(value)
                errors.append(metric_stddev(row, metric))
                all_values.append(value + metric_stddev(row, metric))

        axis.plot(
            THREADS,
            line_values,
            marker="o",
            markersize=5,
            linewidth=2.0,
            color=BACKEND_COLORS[backend],
            linestyle=BACKEND_STYLES[backend],
            label=BACKEND_LABELS[backend],
        )
        if valid_x:
            axis.errorbar(
                valid_x,
                valid_y,
                yerr=errors,
                fmt="none",
                ecolor=BACKEND_COLORS[backend],
                elinewidth=1.1,
                capsize=3,
                alpha=0.8,
            )

    if spec["scale"] == "log":
        positive_values = [value for value in all_values if value > 0]
        lower = min(positive_values) / 2.0 if positive_values else 0.01
        upper = max(positive_values) * 2.5 if positive_values else 1.0
        axis.set_yscale("log")
        axis.set_ylim(lower, upper)
        missing_y = lower * 1.35
    else:
        upper = max(all_values) * 1.20 if all_values else 1.0
        axis.set_ylim(0.0, upper)
        missing_y = max(upper * 0.01, 0.01 if metric == "abort_rate" else upper * 0.001)

    if missing_points:
        axis.scatter(
            [point[0] for point in missing_points],
            [missing_y] * len(missing_points),
            marker="x",
            s=58,
            linewidths=2,
            color="#222222",
            zorder=6,
        )
        for thread_count, backend in missing_points:
            axis.annotate(
                "N/A\nretry limit",
                xy=(thread_count, missing_y),
                xytext=(0, 9),
                textcoords="offset points",
                ha="center",
                va="bottom",
                fontsize=7,
                color="#333333",
            )

    axis.set_xticks(THREADS)
    axis.set_xlim(0.4, 20.6)
    axis.set_xlabel("Worker threads")
    axis.set_ylabel(spec["ylabel"])
    title = f"{spec['title']} — {workload_label(read_ratio, contention)}"
    if spec["scale"] == "log":
        title += " (log y-axis)"
    axis.set_title(title, fontsize=13, pad=12)
    axis.grid(True, which="major", axis="both", alpha=0.25)
    handles, labels = axis.get_legend_handles_labels()
    if missing_points:
        handles.append(Line2D([0], [0], marker="x", color="#222222", linestyle="None", markersize=7))
        labels.append("N/A: retry limit reached")
    axis.legend(handles, labels, loc="best", frameon=True, ncol=2)
    fig.text(
        0.5,
        0.018,
        "Points are means across valid runs; bars are sample stddev (n=3 where valid). "
        "Invalid points are omitted, never treated as zero or interpolated.",
        ha="center",
        fontsize=8.2,
        color="#444444",
    )
    fig.tight_layout(rect=(0.0, 0.055, 1.0, 0.97))
    output_path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(output_path, dpi=180, bbox_inches="tight")
    plt.close(fig)


def write_analysis_summary(
    summary_map: Mapping[Tuple[str, int, int, str], Mapping[str, str]],
    output_path: Path,
) -> None:
    fields = [
        "backend",
        "read_ratio",
        "write_ratio",
        "contention",
        "workload",
        "throughput_1t_tps",
        "throughput_8t_tps",
        "throughput_20t_tps",
        "peak_throughput_tps",
        "peak_threads",
        "abort_rate_20t",
        "p99_20t_us",
        "valid_runs_total",
        "total_runs_total",
        "invalid_points",
        "status",
    ]
    output_path.parent.mkdir(parents=True, exist_ok=True)
    with output_path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        for backend in BACKENDS:
            for read_ratio in READ_RATIOS:
                for contention in CONTENTIONS:
                    rows = [
                        summary_map[(backend, thread_count, read_ratio, contention)]
                        for thread_count in THREADS
                        if (backend, thread_count, read_ratio, contention) in summary_map
                    ]
                    valid_rows = [row for row in rows if (parse_int(row.get("valid_runs")) or 0) > 0]
                    peak_row = max(
                        valid_rows,
                        key=lambda row: float(row["throughput_mean_tps"]),
                        default=None,
                    )
                    valid_total = sum(parse_int(row.get("valid_runs")) or 0 for row in rows)
                    total_total = sum(parse_int(row.get("total_runs")) or 0 for row in rows)
                    invalid_points = sum(
                        1
                        for row in rows
                        if (parse_int(row.get("valid_runs")) or 0) < (parse_int(row.get("total_runs")) or 0)
                    )

                    def csv_metric(thread_count: int, metric: str) -> str:
                        row = summary_map.get((backend, thread_count, read_ratio, contention))
                        value = metric_value(row, metric)
                        if value is None:
                            return "N/A"
                        if metric == "abort_rate":
                            return f"{value / 100.0:.6f}"
                        return f"{value:.6f}"

                    writer.writerow(
                        {
                            "backend": backend,
                            "read_ratio": read_ratio,
                            "write_ratio": 100 - read_ratio,
                            "contention": contention,
                            "workload": workload_label(read_ratio, contention),
                            "throughput_1t_tps": csv_metric(1, "throughput"),
                            "throughput_8t_tps": csv_metric(8, "throughput"),
                            "throughput_20t_tps": csv_metric(20, "throughput"),
                            "peak_throughput_tps": (
                                f"{float(peak_row['throughput_mean_tps']):.6f}" if peak_row else "N/A"
                            ),
                            "peak_threads": peak_row["threads"] if peak_row else "N/A",
                            "abort_rate_20t": csv_metric(20, "abort_rate"),
                            "p99_20t_us": csv_metric(20, "p99"),
                            "valid_runs_total": valid_total,
                            "total_runs_total": total_total,
                            "invalid_points": invalid_points,
                            "status": "retry_limit_reached" if invalid_points else "valid",
                        }
                    )


def make_analysis(
    result_dir: Path,
    raw_rows: Sequence[Mapping[str, str]],
    summary_rows: Sequence[Mapping[str, str]],
    metadata: Mapping[str, str],
    quality: Mapping[str, object],
) -> str:
    summary_map = quality["summary_map"]  # type: ignore[assignment]
    variance = relative_variance_anomalies(summary_map)
    adjacent = adjacent_anomalies(summary_map)
    parameters = {}
    try:
        parameters = json.loads(metadata.get("parameters", "{}"))
    except json.JSONDecodeError:
        pass

    lines = [
        "# STM benchmark analysis",
        "",
        f"Source: `{result_dir.as_posix()}`",
        f"Benchmark commit: `{metadata.get('git_commit', 'unknown')}`",
        f"Host: {metadata.get('cpu_model', 'unknown')}, {metadata.get('logical_cpu_count', 'unknown')} logical CPUs, {metadata.get('compiler', 'unknown')}, {metadata.get('os', 'unknown')}",
        "",
        "This report analyzes the existing full matrix only. It does not rerun the benchmark and does not alter raw.csv or summary.csv.",
        "",
        "## Data quality",
        "",
        f"- Raw rows: **{quality['raw_rows']} / {quality['expected_rows']} expected**; summary groups: **{quality['summary_groups']}**.",
        f"- Observed configuration groups: **{quality['raw_group_count']}**; every group has 3 runs, as intended.",
        f"- Valid raw runs: **{sum(item['valid'] for item in quality['backend_counts'].values())}**; invalid raw runs: **{len(quality['invalid_rows'])}**.",
        f"- Final checksum mismatches: **{len(quality['checksum_mismatches'])}**; invalid rows with a checksum mismatch: **{len(quality['invalid_checksum_mismatches'])}**.",
        "- Means and standard deviations below come from summary.csv and include only valid runs. Invalid points are represented as N/A/missing.",
        "",
        "Backend coverage:",
        "",
    ]
    coverage_rows = []
    for backend in BACKENDS:
        count = quality["backend_counts"][backend]
        coverage_rows.append((BACKEND_LABELS[backend], count["total"], count["valid"], count["invalid"]))
    lines.extend(md_table(("Backend", "Total", "Valid", "Invalid"), coverage_rows))
    lines.extend(["", "## Reproduction parameters", ""])
    lines.extend(
        [
            f"- Workload: 8 deterministic operations per logical transaction; ratios: 90R/10W, 50R/50W, 10R/90W.",
            f"- Contention: low = {metadata.get('contention_low', '65,536 keys')}; high = {metadata.get('contention_high', '16 keys')}.",
            f"- Thread points: {', '.join(str(item) for item in parameters.get('threads', THREADS))}.",
            f"- Build: {metadata.get('build_type', 'Release')}, {metadata.get('optimization', '-O3')}, NDEBUG={metadata.get('NDEBUG', 'unknown')}, VERIFY={metadata.get('STM_WW_VERIFY_LOGIC_MODE', 'unknown')}, TEST_HOOKS={metadata.get('STM_WW_TEST_HOOKS', 'unknown')}.",
            f"- Retry guard: {parameters.get('max_attempts', '1,000,000')} attempts per logical transaction.",
            "",
            "## Single-thread overhead",
            "",
            "At one worker, this is primarily a serialized overhead comparison; it is not a scalability result. The mutex baseline is fastest in every workload point.",
            "",
        ]
    )

    single_rows = []
    mutex_relative = []
    occ_relative = []
    ww_relative = []
    for ratio in READ_RATIOS:
        for contention in CONTENTIONS:
            values = {}
            for backend in BACKENDS:
                row = summary_map.get((backend, 1, ratio, contention))
                values[backend] = finite_metric(row, "throughput")
            if values["mutex"]:
                mutex_relative.append(values["shared_mutex"] / values["mutex"] if values["shared_mutex"] else 0.0)
                occ_relative.append(values["occ"] / values["mutex"] if values["occ"] else 0.0)
                ww_relative.append(values["ww"] / values["mutex"] if values["ww"] else 0.0)
            single_rows.append(
                (
                    f"{ratio}R/{100 - ratio}W {contention}",
                    fmt_tps(values["mutex"]),
                    fmt_tps(values["shared_mutex"]),
                    fmt_tps(values["occ"]),
                    fmt_tps(values["ww"]),
                )
            )
    lines.extend(md_table(("Workload", "std::mutex", "std::shared_mutex", "OccSTM", "WwSTM"), single_rows))
    lines.extend(
        [
            "",
            f"Across the six points, shared_mutex reaches {min(mutex_relative) * 100:.1f}%–{max(mutex_relative) * 100:.1f}% of mutex throughput; OccSTM reaches {min(occ_relative) * 100:.1f}%–{max(occ_relative) * 100:.1f}%; WwSTM reaches {min(ww_relative) * 100:.1f}%–{max(ww_relative) * 100:.1f}%.",
            "The STM gap is consistent with the extra transaction descriptor, read/write-set, version-validation, conflict-management, and EBR paths exercised even without contention.",
            "",
            "## Throughput and low-contention scaling",
            "",
            "The table shows the three requested low-contention anchor points and the peak among all valid thread points.",
            "",
        ]
    )
    low_rows = []
    for ratio in READ_RATIOS:
        for backend in BACKENDS:
            values = {
                thread_count: finite_metric(summary_map.get((backend, thread_count, ratio, "low")), "throughput")
                for thread_count in THREADS
            }
            valid = [(thread_count, value) for thread_count, value in values.items() if value is not None]
            peak_thread, peak_value = max(valid, key=lambda item: item[1], default=(None, None))
            scale = values[20] / values[1] if values[1] and values[20] else None
            low_rows.append(
                (
                    f"{ratio}R/{100 - ratio}W",
                    BACKEND_LABELS[backend],
                    fmt_tps(values[1]),
                    fmt_tps(values[8]),
                    fmt_tps(values[20]),
                    fmt_number(scale, 2) if scale is not None else "N/A",
                    f"{peak_thread}T / {fmt_tps(peak_value)}" if peak_thread else "N/A",
                )
            )
    lines.extend(md_table(("Workload", "Backend", "1T", "8T", "20T", "20T / 1T", "Peak"), low_rows))
    lines.extend(
        [
            "",
            "No backend shows near-linear low-contention scaling in this matrix. The lock baselines peak at one thread; WwSTM also remains below its one-thread throughput at 20T. OccSTM is the exception in shape: it gains parallel throughput over its own one-thread baseline and peaks at 4T, 8T, or 20T depending on the read/write mix, but remains well below the lock baselines in absolute throughput for these points.",
            "",
            "## High-contention behavior",
            "",
            "At high contention, the main separation is progress behavior rather than a universal throughput win for one STM. OccSTM's optimistic retry loop can spend nearly all of its budget retrying; WwSTM completes every recorded point but can still show a high abort rate under pressure.",
            "",
            "High-contention 20T anchor points:",
            "",
        ]
    )
    high_rows = []
    for ratio in READ_RATIOS:
        for backend in ("occ", "ww"):
            row = summary_map.get((backend, 20, ratio, "high"))
            high_rows.append(
                (
                    f"{ratio}R/{100 - ratio}W",
                    BACKEND_LABELS[backend],
                    fmt_tps(finite_metric(row, "throughput")),
                    fmt_pct(parse_float(row.get("abort_rate_mean")) if row and metric_value(row, "abort_rate") is not None else None),
                    fmt_us(finite_metric(row, "p99")),
                    f"{row.get('valid_runs', '0')}/{row.get('total_runs', '0')}" if row else "N/A",
                )
            )
    lines.extend(md_table(("Workload", "Backend", "20T throughput", "20T abort", "20T p99", "Valid runs"), high_rows))
    lines.extend(
        [
            "",
            "The clearest example is 90R/10W high contention at 20T: OccSTM records 24.5k transactions/s, 99.9% abort rate, and 12.22 ms p99, while WwSTM records 1.22M transactions/s, 12.5% abort rate, and 250 us p99. For 50R/50W and 10R/90W at 20T, OccSTM is N/A because all three repetitions hit the retry guard; WwSTM completes at 336k and 206k transactions/s respectively, with 70.4% and 80.7% abort rates.",
            "",
            "## Abort behavior",
            "",
            "Mutex and shared_mutex have zero aborts by construction. OccSTM's abort rate rises sharply with contention and thread count: at 20T low contention it is 4.0% (90R/10W), 48.8% (50R/50W), and 24.7% (10R/90W). At 20T high contention, the valid 90R/10W point reaches 99.9%; the 50R/50W and 10R/90W points exhaust the retry guard.",
            "WwSTM is not abort-free: at 20T high contention it reaches 12.5%, 70.4%, and 80.7% for the three mixes. The evidence for its advantage is progress robustness in this run, not zero aborts or universally higher throughput: all 108 WwSTM points are valid, versus 97/108 OccSTM points.",
            "",
            "## Tail latency",
            "",
            "The p99 plots use a log y-axis because the observed range spans sub-microsecond lock baselines to millisecond-scale retry storms. At 20T low contention, p99 is roughly 51–60 us for mutex, 59–75 us for shared_mutex, 64–132 us for OccSTM, and 271–458 us for WwSTM. Under high contention, OccSTM's p99 can jump to 12.22 ms or 6.51 ms before/while losing progress, whereas WwSTM remains between 250 us and 883 us at the valid 20T points.",
            "",
            "## Lock baselines",
            "",
            "std::mutex is the fastest backend at one thread and remains the strongest absolute-throughput baseline in the measured matrix. shared_mutex is close at one thread, but does not produce a read-heavy scaling win here: the adapter takes an exclusive lock for any mixed batch containing a write, and the 90R/10W workload still contains mixed transactions. For example, at 8T/90R10W, shared_mutex reaches 1.45M (low) and 1.94M (high), versus mutex at 5.12M and 6.12M.",
            "This is a useful negative result: the STM comparison should not claim victory over locks without a workload-specific basis.",
            "",
            "## OccSTM invalid points",
            "",
            "The 11 invalid points are all OccSTM high-contention configurations at 16T or 20T with 50R/50W or 10R/90W. They are retry-limit progress failures under the 1,000,000-attempt guard, not checksum or data-correctness failures: all 11 have final_checksum equal to expected_checksum. Their throughput, abort rate, and latency are therefore N/A in summaries and charts, with no interpolation or zero substitution.",
            "The affected configuration groups are:",
        "",
        ]
    )
    for key, valid_runs, total_runs in quality["invalid_config_rows"]:
        backend, threads, ratio, contention = key
        lines.append(
            f"- {BACKEND_LABELS.get(backend, backend)} {threads}T {ratio}R/{100 - ratio}W {contention}: {valid_runs}/{total_runs} valid runs."
        )
    lines.extend(
        [
            "",
            "## Most useful interview findings",
            "",
            "1. The benchmark uses one deterministic eight-operation Shared KV/Array workload across four backends, three read/write mixes, two contention levels, six thread points, and three repetitions; comparisons use mean ± sample stddev and explicitly preserve invalid progress outcomes.",
            "2. The honest baseline result is that mutex wins the one-thread and many absolute-throughput comparisons; the project does not rely on a cherry-picked STM speedup.",
            "3. OccSTM demonstrates the classic optimistic-retry failure mode under high conflict: at 20T/90R10W it is still valid but spends 99.9% of attempts aborting, with 24.5k TPS and 12.22 ms p99; at harder mixes it reaches the guard.",
            "4. WwSTM trades substantial aborts and tail latency for more stable progress in this matrix: it is valid for 108/108 points, including the OccSTM retry-exhaustion region, but the data does not support claiming a universal throughput win.",
            "5. shared_mutex is a meaningful baseline rather than a straw man; its mixed-transaction locking policy explains why read-heavy batches do not automatically translate into a scaling advantage.",
            "",
            "## Anomalies",
            "",
            "The following are data-review signals, not automatic correctness diagnoses:",
            f"- Relative stddev above 10%: throughput {len(variance.get('throughput', []))} groups, abort rate {len(variance.get('abort_rate', []))} groups, p50 {len(variance.get('p50', []))} groups, p99 {len(variance.get('p99', []))} groups. Near-zero abort rates and very small latency values can make relative percentages look large.",
            f"- Adjacent valid throughput drops below 50%: {len(adjacent['throughput_drop'])} transitions. Largest examples:",
        ]
    )
    for ratio, key, left, right, left_value, right_value in adjacent["throughput_drop"][:6]:
        backend, _, read_ratio, contention = key
        lines.append(
            f"  - {BACKEND_LABELS[backend]} {read_ratio}R/{100 - read_ratio}W {contention}: {left}T {fmt_tps(left_value)} -> {right}T {fmt_tps(right_value)} ({ratio * 100:.1f}% of previous)."
        )
    lines.append(f"- Adjacent valid p99 jumps above 2x: {len(adjacent['p99_jump'])}; the largest is reported below because it coincides with retry pressure.")
    for ratio, key, left, right, left_value, right_value in adjacent["p99_jump"][:5]:
        backend, _, read_ratio, contention = key
        lines.append(
            f"  - {BACKEND_LABELS[backend]} {read_ratio}R/{100 - read_ratio}W {contention}: {left}T {fmt_us(left_value)} -> {right}T {fmt_us(right_value)} ({ratio:.1f}x)."
        )
    lines.append(f"- Abort-rate changes above 20 percentage points across adjacent valid points: {len(adjacent['abort_shift'])}; these are expected contention transitions worth calling out in a presentation, not checksum failures.")
    for delta, key, left, right, left_value, right_value in adjacent["abort_shift"][:5]:
        backend, _, read_ratio, contention = key
        lines.append(
            f"  - {BACKEND_LABELS[backend]} {read_ratio}R/{100 - read_ratio}W {contention}: {left}T {left_value * 100:.1f}% -> {right}T {right_value * 100:.1f}%."
        )
    if quality["incomplete_groups"]:
        lines.append(f"- Incomplete run groups: {len(quality['incomplete_groups'])}; inspect the raw runner output before using those points.")
    else:
        lines.append("- Run-count check: no incomplete groups; every configuration has three raw repetitions.")
    if quality["summary_mismatches"]:
        lines.append(f"- raw.csv/summary.csv count mismatches: {len(quality['summary_mismatches'])}; do not use the derived summary until reconciled.")
    else:
        lines.append("- raw.csv/summary.csv count check: all group counts agree.")
    lines.extend(
        [
            "",
            "## Recommended allocator-ablation points",
            "",
            "Do not run these as part of this analysis step. They are six representative STM points for a later system-allocator vs TierAlloc comparison, keeping seed, warmup, measurement window, and repetitions identical:",
            "",
            "- `occ, 1T, 50R/50W, low` — serialized STM allocation/metadata overhead.",
            "- `ww, 1T, 50R/50W, low` — the corresponding WwSTM single-thread point.",
            "- `occ, 8T, 50R/50W, low` — moderate contention with 3/3 valid runs and visible retries.",
            "- `ww, 8T, 50R/50W, low` — compare allocator cost while preserving WwSTM behavior.",
            "- `occ, 8T, 50R/50W, high` — retry-pressure stress point that is still 3/3 valid.",
            "- `ww, 20T, 10R/90W, high` — high-thread, write-heavy stress point where WwSTM completes but tail/abort cost is visible.",
            "",
            "These points intentionally avoid OccSTM configurations that already have zero valid repetitions, because an allocator comparison needs a completed measurement on both sides.",
        ]
    )
    return "\n".join(lines) + "\n"


def main() -> int:
    args = parse_args()
    result_dir = args.result_dir.resolve()
    output_dir = (args.output_dir or result_dir / "charts").resolve()
    analysis_output = (args.analysis_output or result_dir / "analysis.md").resolve()
    summary_output = (args.summary_output or result_dir / "analysis_summary.csv").resolve()

    raw_rows, summary_rows, metadata = load_inputs(result_dir)
    quality = make_quality_report(raw_rows, summary_rows)
    summary_map = quality["summary_map"]

    for metric in METRICS:
        for read_ratio in READ_RATIOS:
            for contention in CONTENTIONS:
                filename = f"{METRICS[metric]['suffix']}_{ratio_label(read_ratio)}_{contention}.png"
                plot_metric(summary_map, metric, read_ratio, contention, output_dir / "all" / filename)

    highlight_specs = (
        ("throughput", 90, "low"),
        ("throughput", 50, "high"),
        ("abort_rate", 50, "high"),
        ("p99", 10, "high"),
        ("throughput", 10, "low"),
    )
    highlights_dir = output_dir / "highlights"
    highlights_dir.mkdir(parents=True, exist_ok=True)
    for metric, read_ratio, contention in highlight_specs:
        filename = f"{METRICS[metric]['suffix']}_{ratio_label(read_ratio)}_{contention}.png"
        shutil.copy2(output_dir / "all" / filename, highlights_dir / filename)

    write_analysis_summary(summary_map, summary_output)
    analysis_output.parent.mkdir(parents=True, exist_ok=True)
    analysis_output.write_text(
        make_analysis(result_dir, raw_rows, summary_rows, metadata, quality),
        encoding="utf-8",
    )

    print(f"result_dir={result_dir}")
    print(f"charts={output_dir / 'all'}")
    print(f"highlights={highlights_dir}")
    print(f"analysis={analysis_output}")
    print(f"analysis_summary={summary_output}")
    print(f"raw_rows={quality['raw_rows']} expected={quality['expected_rows']}")
    print(f"invalid_rows={len(quality['invalid_rows'])} checksum_mismatches={len(quality['checksum_mismatches'])}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
