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
