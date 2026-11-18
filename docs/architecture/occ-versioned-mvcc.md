# OccSTM versioned stripes + bounded MVCC (2026-11-18)

> Correctness-oriented revision of CaSTM OccSTM. This is a TL2-inspired
> multi-version extension, **not** a verbatim implementation of TL2 or SMV,
> and is not asserted to have a full formal opacity/serializability proof.

## Problem and deterministic pre-fix reproduction

In the prior code, `GlobalClock::tick()` occurred before sequentially updating
the heads for all variables of a transaction. An unlocked read-only transaction
could observe and **successfully accept** a mixed state from a single writer:
A=1, B=0 while a write transaction intended A=1, B=1.

The regression `OccAtomicVisibilityTest.ReadOnlyRejectsPartialMultiVariableCommit`
replaces the first WriteSet committer with a test-only pause hook, then runs a
read-only transaction during the pause. It failed on the baseline
(`a83773d`) with `accepted_torn_snapshot: Actual true, Expected false`.
[Pre-fix CI / raw failure](https://github.com/ZephyrVia/CaSTM/actions/runs/44237314399).

## Invariants and implementation

1. `StripedLockTable` stores `(committed_version << 1) | locked` as a single
   `std::atomic<uint64_t>`, with sorted, unique lock acquisition. Address-hash
   collisions remain possible and are conservative false conflicts.
2. A writing transaction takes **all** its write stripes before calling
   `GlobalClock::tick()`. Each production committer only links a fully
   prepared VersionNode to its TMVar head and is `noexcept`.
3. Writers release **no** write stripe until **every** new head has been
   published. Successful unlock atomically publishes `(WV, unlocked)`.
   Abort unlock preserves the previous committed stripe version.
4. Shared loads check their stripe state both before and after copying the
   immutable EBR-protected value. A locked/changed state triggers a retry.
   A stable read may traverse `prev` to select the newest retained node with
   `write_ts <= RV`; unlike pure TL2, historical reads remain supported.
   Readers encountering a writer will currently retry rather than read
   through the active writer's lock.
5. Update transactions conservatively validate the ReadSet under their own
   write locks: external read stripes cannot be locked, their committed
   stripe versions cannot exceed RV, and each previously selected read node
   must still be the current TMVar head. This may reject a safe historical
   update, but preserves the intentionally strict conflict policy.
6. `TMVar::pruneNoThrow` runs after *all* version publications, under the
   writer's stripe lock and before unlock. It preallocates a GarbageNode
   **before** detaching the old history tail. A failed allocation simply
   retains more history; no detached chain is lost and the transaction
   cannot become half-published due to `retire()` allocation failure.
   `EBRManager::retirePrepared()` enqueues a preallocated record.
7. A failed WriteSet vector growth now deletes its unpublished draft node.

The EBR critical section still protects old node lifetimes, and the maximum
retained history length remains a target of `MAX_HISTORY=8`. If a required
historical version has already been pruned, reads retry with
`SnapshotUnavailable` as before. No unbounded-history guarantee is made.

## Validation

- Original torn-snapshot regression: **fails on original code**;
  **passes after versioned lock/read-barrier changes**.
- New `RetainedHistoricalSnapshotAcrossMultiVarCommit`: an RV captured
  before a two-key commit still selects both old versions afterwards.
- New `DisjointWriterProgressWhileFirstIsPaused`: another writer on a
  different stripe finishes while the multi-key writer is intentionally
  paused after its first head is published, confirming there is no global
  commit mutex.
- Existing OccSTM, EBR, TierAlloc and WwSTM regressions continue to run,
  including targeted OccSTM/EBR TSan repetitions and ASan+UBSan (with leak
  detection disabled, as documented by the CI).
- The existing `ConcurrentSnapshotTraversalAndPruning` pressure test now
  accepts `ReadLocked` as a legitimate retry in addition to
  `SnapshotUnavailable`; its payload/checksum assertions are unchanged.

## Short same-runner performance A/B

Source: [GitHub Actions A/B](https://github.com/ZephyrVia/CaSTM/actions/runs/44239016262)
and [raw artifact](https://github.com/ZephyrVia/CaSTM/actions/runs/44239016262/artifacts/14399086276).

- Baseline: `7ceb0a73` (pre-revision; contains the torn-snapshot bug).
- Candidate for this comparison: `a2e4db44` (changes after this are
  documentation/CI cleanup only).
- Ubuntu 22.04 GitHub-hosted runner, Release, `STM_ALLOCATOR_MODE=system`.
- `--backend occ --warmup-ms 200 --measure-ms 500 --csv`,
  3 alternating runs for each side/workload.
- All 30 samples reported `valid=1`. They are short smoke comparisons,
  **not** long-run performance proofs.

| Threads | Read ratio | Contention | Baseline TPS | Candidate TPS | Candidate / baseline |
|---:|---:|---|---:|---:|---:|
| 1 | 90% | Low | 912,510 | 810,207 | 0.888 |
| 4 | 90% | Low | 2,512,875 | 2,266,711 | 0.902 |
| 1 | 50% | High | 1,285,303 | 1,209,274 | 0.941 |
| 4 | 50% | High | 806,550 | 696,375 | 0.863 |
| 4 | 10% | High | 480,651 | 474,292 | 0.987 |

The added read barriers and stronger atomic ordering have measurable costs,
especially for read-intensive workloads. Avoid treating the unsafe baseline's
throughput as a correct-STM performance target. Investigate lock hot spots
and memory-order costs only with separate correctness tests and a larger
matched A/B matrix.

## Scope and remaining work

- No formal proof that the TL2-inspired MVCC protocol provides opacity or
  serializability for all possible user payload types and thread schedules.
- No deterministic fault injection of EBR retirement-record OOM; the code
  orders allocation before detach to make that failure non-destructive.
- Under the current protocol, write transactions based on non-head historical
  versions are intentionally aborted; blind writes remain allowed.
- Repeated writes to the same TMVar in a single transaction may create
  multiple nodes with one commit timestamp. Potential coalescing is separate.
- `Transaction::free` and `Transaction::alloc` transaction-lifetime
  semantics are not changed by this patch and merit independent review.
- Full-repository TSan, LSan, long-running benchmark and formal
  multi-version correctness review remain outstanding.
