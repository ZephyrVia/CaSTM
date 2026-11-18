# OccSTM: TL2-style versioned StripedLock + bounded MVCC

> Scope: CaSTM OccSTM correctness refactor, branch `codex/occ-mvcc-versioned-lock-20261118`.
> This describes the **implemented protocol**, not a claim that the original TL2/SMV formal proofs apply unchanged.
> The frozen `v0.2.0-prototype` tag is not modified.

## Motivation and baseline failure

Old `Transaction::commit()` obtained a new `GlobalClock` WV before publishing
multiple `TMVar` heads, while `Transaction::load()` did not examine a
versioned writer lock. Read-only `commit()` returned success without further
validation. A phase-gated test (`OccAtomicVisibilityTest.NeverAcceptsPartiallyPublishedMultiVarWrite`)
paused a writer between publishing A=1 and B=1 and demonstrated a read-only
transaction **successfully accepting (A=1, B=0)**.

Baseline CI: https://github.com/ZephyrVia/CaSTM/actions/runs/44235607188
The failing assertion is the *expected reproduction*, not a successful test.

## Data model

- `GlobalClock`: monotonically increasing version source for transaction RV
  (at `begin`) and WV (after acquiring *all* write stripes at `commit`).
  It is **not** an EBR epoch or an `is_committed` clock.
- `StripedLockTable`: each of 2^20 stripes stores a 64-bit atomic pair
  `(last_committed_write_version << 1) | locked_bit`.
  One stripe may cover several TMVars; false conflicts are possible.
- `TMVar<T>`: one atomic head pointing to a chain of immutable `VersionNode`
  objects, each with `write_ts`, atomic `prev`, and `payload`.
- `EBRManager`: protects already loaded nodes from physical reclamation
  while the enclosing transaction guard remains alive.
- `ReadSet`: records the version **selected** during a read;
  an *updating* transaction must validate that it is still the current head.
- `WriteSet`: stores private draft VersionNodes and their non-throwing
  publication callbacks.

## Read-only MVCC protocol

For a shared read not satisfied by Read-Your-Own-Writes:

1. Load the stripe state (`before`) with acquire; retry if locked.
2. Load the TMVar head (acquire), traverse `prev` while `write_ts > RV`,
   and select the newest retained version `write_ts <= RV`.
3. Copy the selected immutable payload; retry if no such version exists.
4. Issue an acq_rel fence and load the stripe state again (`after`).
   Retry if it is now locked or `before != after`.
5. Only after a successful read barrier, append a ReadSet record.

Unlike single-version TL2, an *unlocked* stripe version may be newer than RV:
this is permitted for read-only transactions, as long as a suitable historical
VersionNode remains. Encountering an active writer still causes a conservative
retry even when an old version is available. A read-only `commit()` does
not revalidate because each shared read has been checked against its fixed RV.

## Updating transaction protocol

1. Prepare all drafts and the sorted, deduplicated write stripe indices.
   Dynamic allocation is allowed **before** the critical publication phase.
2. Acquire **all** write stripes in increasing index order.
3. Obtain a unique WV from `GlobalClock::tick()`, *after* all locks.
4. Validate every ReadSet entry: reject a stripe locked by another writer,
   a committed stripe version above RV, an entry whose selected version is
   no longer the TMVar head, or any stripe state change during validation.
   The updating path is intentionally strict (may reject historical reads).
5. Publish all new TMVar heads with WV using only `noexcept` operations.
   Once the first head is published, the transaction cannot logically abort.
6. Best-effort version-chain pruning runs with the stripes still held. An EBR
   `GarbageNode` ledger is allocated with `new (std::nothrow)` **before**
   severing a chain edge; on failure, history is simply retained longer.
   After severing, `retirePrepared()` enqueues the ledger without another
   heap allocation.
7. Release each stripe with the new unlocked WV using a release store.
   Failed validation restores the prior lock version via RAII.

**No process-wide commit mutex is introduced.** Different stripes may commit
concurrently; their WV and completion orders need not match.

## Safety argument and boundaries

- A writer takes **all** its stripes before drawing WV and keeps them until
  *all* affected heads are published. Thus a reader whose RV includes an
  unfinished writer's WV cannot accept a value from that writer's stripe
  without observing the held lock / changed version.
- A writer that starts after a reader's RV gets a larger WV, so the reader
  can select the older immutable VersionNode. The read barrier rules out
  accepting a torn read during a concurrent head modification.
- Readers may retry when a writer holds a stripe; bounded history
  (`MAX_HISTORY=8`, up to nine retained nodes in the common case) may make
  old snapshots unavailable.
- An updating transaction *cannot* commit based on a historical non-head
  version, preserving conservative OCC read/write conflict handling.
- The algorithm borrows the TL2 versioned-lock discipline but extends its
  read rule with MVCC history; formal opacity/serializability for arbitrary
  program histories has **not** been mechanically proved. Tests provide
  bounded empirical evidence, not a proof of unrestricted execution histories.
- The `T` payload must be immutable after node publication; callers must keep
  the EBR guard across all transaction reads. External writes to payloads,
  direct unsafe head dereferences, and destruction of TMVars with active users
  are outside this protocol.
- Unrelated TMVars hashing to the same stripe may still cause false conflicts.
  This design does not guarantee no starvation, a wait-free read, or perpetual
  historical snapshot retention.
- Duplicate stores to the same TMVar inside one transaction still create
  multiple private drafts and may publish successive nodes with the same WV.
  Their last write wins; coalescing them is separate future optimization.
- EBR `retirePrepared` assumes its internal queue's mutex remains operable;
  unrecoverable platform failures terminate rather than unwinding after a
  chain has been detached.

## Regression suite

- `OccAtomicVisibilityTest.NeverAcceptsPartiallyPublishedMultiVarWrite`:
  deterministic baseline failure, now rejection of a mixed snapshot.
- `OccAtomicVisibilityTest.DisjointWritersMayCommitOutOfTimestampOrder`:
  disjoint stripes can complete out of WV order; a reader with RV covering
  an unfinished WV must not accept that writer's partial state.
- `OccAtomicVisibilityTest.ConcurrentHistoricalSnapshotPairsRemainConsistent`:
  multi-threaded read-only snapshot pairs with concurrent two-variable updates.
- Existing `OccTransactionTest.ReadsRetainedVersionAfterAnotherCommit`:
  a retained old version remains readable after a new commit.
- Existing `OccTransactionTest.ChangedReadAbortsWithoutPublishingAndCanRetry`:
  strict writer validation prevents stale dependencies from committing.
- Existing `OccReclamationTest.*`: historical trimming and EBR lifetime.

Read barriers may now produce `ReadLocked` / `ReadVersionChanged`
retries during a read itself, in addition to `SnapshotUnavailable`.

Reference for standard TL2 (not this full MVCC extension):
https://people.csail.mit.edu/shanir/publications/Transactional_Locking.pdf
