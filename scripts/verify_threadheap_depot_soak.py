#!/usr/bin/env python3
"""Verify 10-minute real OccSTM/EBR thread-churn experiment and heap reuse."""
import csv
import json
from pathlib import Path
import statistics
import sys

root = Path(sys.argv[1])
result = json.loads((root / "depot-soak-result.json").read_text())
rows = list(csv.DictReader((root / "depot-soak-samples.csv").open()))
assert result["passed"] is True, result
assert result["allocator"] == "native" and result["version_allocator"] == "tier"
assert result["threads"] == 20 and result["phase_seconds"] == 600
assert len(result["phases"]) == 1
phase = result["phases"][0]
assert phase["phase"] == "thread_churn" and phase["passed"] and phase["errors"] == 0
assert phase["work_seconds"] >= 600
assert phase["batches"] >= 100 and phase["threads_started"] == 20 * phase["batches"]
assert phase["threads_started"] >= 2000
assert phase["writes"] > 0 and phase["writes"] == phase["checksum"] == phase["expected_checksum"]
assert phase["created"] == phase["destroyed"]
assert phase["retired"] == phase["reclaimed"]
assert phase["unpublished"] == 0
assert phase["teardown_destroyed"] == phase["retained_before_teardown"]
batches = [r for r in rows if r["phase"] == "thread_churn" and r["event"] == "batch_joined"]
assert len(batches) == phase["batches"], (len(batches), phase["batches"])
assert all(r["check_ok"] == "1" and r["quiescent"] == "1" and int(r["errors"]) == 0 for r in batches)

vms = [int(r["vms_kib"]) / 1024 for r in batches]
rss = [int(r["rss_kib"]) / 1024 for r in batches]
heaps = [int(r["depot_created"]) for r in batches]
reuse = [int(r["depot_reuse"]) for r in batches]
idle = [int(r["depot_idle"]) for r in batches]
leased = [int(r["depot_leased"]) for r in batches]
assert all(x == 20 for x in idle), set(idle)
assert all(x == 1 for x in leased), set(leased)
assert all(x == idle[i] + leased[i] for i,x in enumerate(heaps))
assert all(heaps[i] <= heaps[i+1] for i in range(len(heaps)-1))
assert heaps[-1] - heaps[0] <= 2, (heaps[0], heaps[-1])
assert reuse[-1] - reuse[0] >= phase["threads_started"] - 40
assert vms[-1] - vms[0] <= 64, (vms[0], vms[-1])

first = min(12,len(vms))
last = min(12,len(vms))
summary = {
    "schema_version":1,
    "candidate_commit":(root/"candidate-commit.txt").read_text().strip(),
    "workload":"10-minute OccSTM/EBR 20-thread thread-churn, 5s batches",
    "valid": True,
    "seconds":phase["work_seconds"],
    "batches":phase["batches"],
    "workers_started":phase["threads_started"],
    "transactions":phase["transactions"],
    "writes_and_checksum":phase["writes"],
    "retired_and_reclaimed":phase["retired"],
    "versions_created_and_destroyed":phase["created"],
    "vms_first_mib":vms[0],
    "vms_last_mib":vms[-1],
    "vms_growth_mib":round(vms[-1]-vms[0],3),
    "vms_early_median_mib":statistics.median(vms[:first]),
    "vms_late_median_mib":statistics.median(vms[-last:]),
    "rss_first_mib":rss[0],
    "rss_last_mib":rss[-1],
    "rss_peak_mib":max(rss),
    "heap_created_first":heaps[0],
    "heap_created_last":heaps[-1],
    "heap_created_growth":heaps[-1]-heaps[0],
    "heap_reuses_first":reuse[0],
    "heap_reuses_last":reuse[-1],
    "heap_leased_last":leased[-1],
    "heap_idle_last":idle[-1],
    "limits":{"max_vms_growth_mib":64, "max_heap_growth":2},
}
(root/"depot-soak-summary.json").write_text(json.dumps(summary,indent=2)+"\n")
print(json.dumps(summary,indent=2))
