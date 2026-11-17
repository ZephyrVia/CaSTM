#!/usr/bin/env python3
"""Compare two identical OccSTM/EBR churn workloads under separate allocators."""
import csv
import json
from pathlib import Path
import statistics
import sys

root = Path(sys.argv[1])
results = {}

for mode in ("baseline", "depot"):
    result = json.loads((root / f"{mode}.json").read_text())
    rows = list(csv.DictReader((root / f"{mode}.csv").open()))
    assert result["passed"] is True, (mode, result)
    assert len(result["phases"]) == 1, mode
    phase = result["phases"][0]
    assert phase["phase"] == "thread_churn", mode
    assert phase["passed"] is True and phase["errors"] == 0, phase
    assert phase["batches"] >= 20 and phase["threads_started"] == result["threads"] * phase["batches"], phase
    assert phase["checksum"] == phase["writes"] and phase["writes"] > 0, phase
    assert phase["created"] == phase["destroyed"], phase
    assert phase["retired"] == phase["reclaimed"], phase
    assert phase["unpublished"] == 0, phase
    assert phase["work_seconds"] >= result["phase_seconds"], phase
    batch = [r for r in rows if r["event"] == "batch_joined" and r["quiescent"] == "1"]
    assert len(batch) == phase["batches"], (len(batch), phase["batches"])
    assert all(r["check_ok"] == "1" and int(r["errors"]) == 0 for r in batch), mode

    vms = [int(r["vms_kib"]) / 1024 for r in batch]
    rss = [int(r["rss_kib"]) / 1024 for r in batch]
    n = min(5, len(vms))
    metrics = dict(
        commit=result["source_commit"] if "source_commit" in result else mode,
        valid=True,
        seconds=phase["work_seconds"],
        batches=phase["batches"],
        threads_started=phase["threads_started"],
        transactions=phase["transactions"],
        writes=phase["writes"],
        retired=phase["retired"],
        reclaimed=phase["reclaimed"],
        checksum=phase["checksum"],
        vms_first_mib=vms[0],
        vms_last_mib=vms[-1],
        vms_growth_mib=round(vms[-1] - vms[0], 3),
        vms_early_median_mib=statistics.median(vms[:n]),
        vms_late_median_mib=statistics.median(vms[-n:]),
        rss_first_mib=rss[0],
        rss_last_mib=rss[-1],
        rss_peak_mib=max(rss),
    )
    if mode == "depot":
        created = [int(r["depot_created"]) for r in batch]
        reused = [int(r["depot_reuse"]) for r in batch]
        leased = [int(r["depot_leased"]) for r in batch]
        idle = [int(r["depot_idle"]) for r in batch]
        assert created[-1] == leased[-1] + idle[-1], (created[-1], leased[-1], idle[-1])
        metrics.update(
            depot_heaps_created_first=created[0],
            depot_heaps_created_last=created[-1],
            depot_heaps_created_growth=created[-1] - created[0],
            depot_reuses_first=reused[0],
            depot_reuses_last=reused[-1],
            depot_reuse_growth=reused[-1] - reused[0],
            depot_leased_last=leased[-1],
            depot_idle_last=idle[-1],
        )
    results[mode] = metrics

summary = {
    "schema_version": 1,
    "experiment": "CaSTM OccSTM real transaction / EBR thread churn A-B",
    "duration_per_variant_s": results["depot"]["seconds"],
    "baseline": results["baseline"],
    "depot": results["depot"],
    "vm_growth_reduction_mib": round(
        results["baseline"]["vms_growth_mib"] - results["depot"]["vms_growth_mib"], 3
    ),
    "caution": "Short-run CI on GitHub-hosted runner, not the original WSL2 60-minute workload. Allocator heap count != OS virtual memory. Dedicated TSan and full soak remain separate.",
}
(root / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
print(json.dumps(summary, indent=2))
