# ThreadHeapDepot: safe ThreadHeap handoff

The depot manages the **lifetime of per-thread heap managers**, while
CentralHeap manages the pool of 2 MiB Chunks. They solve different problems.

## Design

- The TLS value is a `ThreadHeapLease`, not a `ThreadHeap`.
- Every actual `ThreadHeap` is constructed at a stable address and kept for
  the lifetime of the process. This stabilizes every `Slab::owner_` pointer.
- A process-wide `ThreadHeapDepot` keeps an intrusive LIFO of idle managers.
  `acquire()` and `park()` use a mutex, but normal small-block allocations
  still operate on the leased heap without a global mutex.
- Park transfers **exclusive management rights**, not ownership of Slab
  allocations. Neither active objects nor Slab metadata is destroyed.
- `Slab::remote_free_list_` stays atomic and accepts remote frees during the
  idle period. The next lease owner can reclaim these free blocks.
- Counts of created, leased and idle heaps, plus reuse events, are exposed for
  diagnosing thread churn. Heap creation follows peak concurrent leases rather
  than cumulative thread creation under normal operation.

## Safety limitations

- A parked heap can contain live `VersionNode` and EBR-retired objects.
  Returning its Chunk to CentralHeap would recreate BUG-01
  (`RETURN-LIVE-CHUNK`), so this feature never performs that return.
- There is no background scavenger for orphan slabs. Full reclamation would
  require protection against in-flight `freeRemote()` and EBR references.
- The process-lifetime depot is deliberately not destroyed while global
  shutdown and TLS destructor ordering could leave published object pointers.
- The TLS destructor order for other user-defined thread-local objects that
  call the allocator is a separate audit item; such objects should not try
  to use TierAlloc after the ThreadHeapLease for that thread has been destroyed.
- EBR retains responsibility for deciding **when** an object can be released.
  The depot controls only **which thread** may manipulate a heap's local state.

## Tests

`ThreadHeapDepotTest.*` covers sequential thread churn, published objects
surviving the old owner, remote free while idle and subsequent reuse,
concurrent remote frees and new-owner allocations, and full/partial Slab
continuity. It should be evaluated under normal, sanitizer and long-duration
reclamation testing before deployment.
