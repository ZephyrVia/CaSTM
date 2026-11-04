# STM performance benchmark

This directory contains the first reproducible benchmark for CaSTM.  It is
intentionally separate from the correctness tests: the benchmark uses one
common Shared KV/Array workload and adapts only the transaction boundary to
each backend.

The recorded results, original CSV snapshots, experiment commits and hashes are
available in the [performance report](../docs/performance/README.md).
The current correctness-test scope is documented [separately](../docs/testing.md).

## Recorded environment

Before this benchmark was added, the repository had no reusable benchmark
harness, no Google Benchmark integration, and no standalone performance
example.  The only timing code was an ad-hoc timer in an older OccSTM tree
test.  OccSTM and WwSTM expose different transaction types, so the harness
uses a small backend adapter rather than pretending that their high-level APIs
are identical.

The archived experiments used 20 logical CPUs (Intel Core i5-14600KF), GCC 11.4,
and WSL2. The documented build commands have been checked with CMake 3.22.
The runner records the actual host information in
`metadata.txt` for every result directory instead of treating these values as
portable constants.

## Workload and fairness

Each logical transaction performs eight deterministic operations by default.
Every operation chooses a key uniformly through a local splitmix64 generator;
the generator is seeded by backend-independent `(run, worker, logical_id)`
coordinates, so retries replay the same operation batch.  A read returns the
current value.  A write reads the value and stores `value + 1`.  The final
aggregate must equal the number of committed write operations, including the
warmup phase; otherwise the run is marked `valid=0` and is excluded from
summary statistics.

The default workload points are:

- `90R/10W`, `50R/50W`, and `10R/90W`;
- low contention: 65,536 keys;
- high contention: 16 keys;
- `ops_per_tx`, key counts, seed, and retry cap are CLI-overridable.  The
  default retry cap is 1,000,000 attempts per logical transaction: it is a
  hang guard, not a successful-transaction shortcut.

The four backends are:

- `mutex`: one global exclusive `std::mutex` for the whole logical transaction;
- `shared_mutex`: shared lock for an all-read batch, exclusive lock for a
  batch containing any write;
- `occ`: manual `Occ::Transaction` retry loop around the common operation
  batch, with `RetryException` and failed commit counted as aborts;
- `ww`: manual `Ww::TxContext` retry loop around the same batch, with wounded
  or failed attempts counted as aborts.

The `shared_mutex` definition is deliberately documented: a mixed transaction
uses an exclusive lock, so it is a fair baseline for the same atomic unit but
does not claim that individual reads inside a mixed transaction are parallel.

The main historical matrix keeps native allocator choices: OccSTM VersionNode
uses TierAlloc, while WwSTM VersionNode/WriteRecord use the system allocator.
It compares complete implementations, not an isolated conflict-algorithm variable.
Use allocator ablation below for the separate allocation-path comparison.

## Metrics

Throughput is committed logical transactions per second.  `abort_rate` is
`aborted attempts / all attempts`; a retry that eventually commits still
contributes its failed attempts.  Mutex baselines have zero aborts.

Latency measures the complete logical transaction, including STM retries, but
excludes deterministic operation-batch construction.  To avoid turning the
measurement into a latency recorder benchmark, one successful logical
transaction per 256 is sampled per worker.  p50 and p99 are computed from the
merged samples.  Every backend also folds read values into a thread-local sink;
the sink is emitted as `read_checksum` so pure reads remain observable to the
compiler.  The measurement window starts only after all workers have left the
warmup phase; thread creation and startup are not included.

## Build

Use a separate Release build.  The target itself enforces `-O3`, `NDEBUG`,
`STM_WW_VERIFY_LOGIC_MODE=0`, and `STM_WW_TEST_HOOKS=0`.  `-march=native` is
opt-in through `-DSTM_BENCHMARK_NATIVE=ON` and is not required for portable
results.

```bash
cmake -S . -B /tmp/castm-bench-release -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/castm-bench-release --target stm_benchmark -j2
```

A single point can be inspected as CSV:

```bash
/tmp/castm-bench-release/benchmarks/stm_benchmark \
  --backend ww --threads 8 --read-ratio 50 --contention high \
  --warmup-ms 1000 --measure-ms 5000 --run 1 --csv
```

## Runner and result artifacts

The runner starts each backend/configuration in its own process, writes
`raw.csv`, `summary.csv`, and `metadata.txt` under
`bench/results/<UTC timestamp>/`, and continues after a failed point so that
the failure remains visible as `valid=0`.  Invalid rows never contribute to
`summary.csv` means or standard deviations. Historical invalid rows only record
`exit code 1`; the limits of retry-exhaustion attribution are discussed in the
[report](../docs/performance/README.md#无效运行). Checksum equality validates the
workload's final-sum invariant, not every transactional isolation property.

The two historical raw/summary CSV pairs are intentionally archived under
`docs/performance/data/`. New results and bulky local analysis artifacts remain
under the ignored `bench/results/` directory.

Representative smoke matrix (1/4/8 threads, 90R low/high and 50R high):

```bash
python3 scripts/run_stm_benchmark.py \
  --binary /tmp/castm-bench-release/benchmarks/stm_benchmark \
  --smoke --warmup-ms 100 --measure-ms 300 --repetitions 1
```

Full first matrix (the runner caps thread points to the host; on the recorded
20-logical-CPU machine this is 1/2/4/8/16/20, six workload points, three
repetitions):

```bash
python3 scripts/run_stm_benchmark.py \
  --binary /tmp/castm-bench-release/benchmarks/stm_benchmark \
  --warmup-ms 1000 --measure-ms 5000 --repetitions 3
```

For a previously generated raw file, summary generation is also standalone:

```bash
python3 scripts/summarize_stm_benchmark.py \
  bench/results/<timestamp>/raw.csv \
  --output bench/results/<timestamp>/summary.csv
```

Do not run the benchmark with sanitizers or VERIFY mode, and do not interpret
one run as a performance conclusion.  Check validity, retry rate, variance,
single-thread overhead, and scaling trends first.

## Allocator ablation

Build two separate Release binaries from the same source revision:

~~~bash
cmake -S . -B /tmp/castm-allocator-system \
  -DCMAKE_BUILD_TYPE=Release -DSTM_ALLOCATOR_MODE=system
cmake --build /tmp/castm-allocator-system --target stm_benchmark -j2
cmake -S . -B /tmp/castm-allocator-tier \
  -DCMAKE_BUILD_TYPE=Release -DSTM_ALLOCATOR_MODE=tier
cmake --build /tmp/castm-allocator-tier --target stm_benchmark -j2
python3 scripts/run_allocator_ablation.py \
  --system-binary /tmp/castm-allocator-system/benchmarks/stm_benchmark \
  --tier-binary /tmp/castm-allocator-tier/benchmarks/stm_benchmark \
  --warmup-ms 1000 --measure-ms 5000 --repetitions 5
~~~

The runner fixes six configurations and records five independent processes for
each allocator/configuration pair: 60 runs in total. The switch covers key STM
allocation paths, not TxDescriptor, EBR metadata or all standard-library buffers.
Only compare speedup where both sides have all five valid runs.

The main matrix was recorded at `4c47011`, and the allocator experiment at
`6998448`; exact hashes and parameters are in
[provenance.json](../docs/performance/data/provenance.json).
Runs on the current revision are new measurements, not replacements for those
snapshots. For an offline check of archived hashes, validity and summary
statistics, use the [report's verification command](../docs/performance/README.md#复核与重新运行).

## Retry diagnostics and OccSTM backoff

`Transaction::lastAbortReason()` reports the first detected OccSTM failure in
an attempt: `SnapshotUnavailable`, `ReadLocked`, or `ReadVersionChanged`.
`begin()` clears the reason. A locked read is a conflict on the **lock stripe**;
this counter alone cannot distinguish a true key conflict from a stripe alias,
nor does it measure lock-wait time. WwSTM records body inactivity and commit
failure separately without changing its conflict policy.

CSV now includes terminal failure reason/phase/worker, measured attempts and
aborts, failed logical transactions, retry histogram percentile upper bounds,
maximum observed retries, warmup totals and per-cause abort counts. Counts are
recorded **before** a failed logical transaction stops the run. All attempts,
including the final failing attempt, count toward the totals. A retry is an
attempt after the first: `max(0, attempts - 1)`, for success or terminal failure.

Use `--diagnostics-json PATH` to retain each worker's warmup and measurement
counters, failure information and retry histogram. Bucket 0 represents zero
retries; bucket `i > 0` represents `[2^(i-1), 2^i-1]` retries. CSV
`retries_p50_upper`/`retries_p99_upper` use the bucket upper endpoint, capped by
the observed maximum; they are **not exact percentiles**. Final-sum checks still
include successful writes from both phases. Latency samples still cover only
successful measured logical transactions, once per 256 per worker; a zero
reported latency can mean there were no samples. Terminal failures have no
latency sample. Each abort is assigned one first-observed cause, not all
conditions that might also have failed.

The runner distinguishes retry exhaustion, caught allocation/other exceptions,
checksum mismatch, report I/O failure, timeout, signal, nonzero process exit,
launch failure and malformed CSV. It preserves the binary's error text and
stderr. A nonzero exit always invalidates a row. A killed/timed-out process may
have no diagnostic file or counters; those fields remain empty. This is
observability, not a guarantee of recovery from arbitrary allocation failures
inside the STM protocol. Metadata includes a binary SHA-256, dirty-tree flag
and source-file hashes; an uncommitted experiment is not attributed solely to
its HEAD commit.

The benchmark default remains `--backoff none` as the reference retry loop.
For OccSTM, `yield` yields after every abort; `jitter` yields after the first two
consecutive aborts, then uses randomized exponential delays from 0 through
1, 2, 4, 8, 16, 32 and 64 microseconds, capped at 64. Zero delay yields.
Scheduling can exceed that requested cap. The delay runs after write locks
have been released and only when another attempt is allowed. The failure count
resets for each logical transaction; RNG state is per worker and independent
of the workload RNG. Non-default policies are rejected for other backends.

The public `STM::atomically(callback)` now uses this same jitter policy for
both failed validation and missing history. An optional second argument
`STM::Occ::BackoffPolicy::None` or `Yield` selects another policy. Both the API
and benchmark retain their existing logical-transaction EBR guard across
retries; sleeping readers can delay reclamation. This change does not establish
bounded memory use or starvation freedom.

```bash
/tmp/castm-bench-release/benchmarks/stm_benchmark \
  --backend occ --backoff jitter --threads 20 --read-ratio 50 --contention high \
  --warmup-ms 300 --measure-ms 1500 --max-attempts 1000000 \
  --diagnostics-json /tmp/occ-diagnostics.json --csv

python3 scripts/run_retry_ablation.py \
  --binary /tmp/castm-bench-release/benchmarks/stm_benchmark \
  --output-dir bench/results/occ-retry-ablation \
  --threads 1,20 --policies none,yield,jitter --repetitions 3 \
  --warmup-ms 300 --measure-ms 1500 --max-attempts 1000000
```

The ablation launches independent processes and rotates policy order for each
repetition of a workload. All policies use the same binary, allocator, seed,
operation count and attempt budget. `summary.csv` groups by `backoff` when
present, retaining invalid-run counts. The older plotter rejects mixed policies
rather than accidentally combining them. See the
[diagnosis and ablation report](../docs/performance/occ-retry-diagnostics.md) for
the pre-fix experiment. The [post-fix report](../docs/performance/occ-postfix-performance.md)
reruns the same 108-process matrix on clean commit `9bccd33` after all four CI
jobs passed. Its 103 valid runs and five retry-limit failures are archived
separately; historical batches do not isolate the cost of the atomic-edge fix.

Runner regression checks, including short binary counter-reconciliation tests:

```bash
STM_BENCHMARK_BIN=/tmp/castm-bench-release/benchmarks/stm_benchmark \
  python3 -m unittest discover -s tests/scripts -v
```
