#include "OccSTM/Transaction.hpp"
#include "OccSTM/StripedLockTable.hpp"

#include <algorithm>
#include <atomic>
#include <vector>

namespace STM {
namespace Occ {

Transaction::Transaction(TransactionDescriptor* desc) : desc_(desc) {}
Transaction::~Transaction() {}

bool Transaction::commit() {
    auto& wset = desc_->writeSet();

    if (wset.empty()) {
        // The per-read versioned lock protocol protects this fixed-RV
        // historical snapshot, so no commit-time validation is needed.
        desc_->reset();
        return true;
    }

    // All potential vector allocations and sort work occur before the
    // first write stripe is acquired.
    lockWriteSet();
    // WV is allocated ONLY AFTER every write stripe is locked.
    const uint64_t wv = GlobalClock::tick();

    if (!validateReadSet()) {
        unlockWriteSet();
        return false;
    }

    // Publishing must not fail after the first head is updated.
    // Typed committers are noexcept and only link preallocated nodes.
    for (auto& entry : wset) {
        entry.committer(entry.tmvar_addr, entry.new_node, wv);
        entry.new_node = nullptr;
    }

    // Defer EBR enrollment until ALL heads have been published. Pruning
    // reserves retirement records first; OOM leaves a longer version chain.
    for (auto& entry : wset) {
        if (entry.pruner) entry.pruner(entry.tmvar_addr);
    }

    // A writer's stripe unlock publishes (WV, unlocked) atomically.
    // Never release any write stripe until all heads are published.
    unlockCommittedWriteSet(wv);
    desc_->commitAllocations();
    desc_->reset();
    return true;
}

bool Transaction::validateReadSet() {
    const uint64_t rv = desc_->getReadVersion();
    auto& lock_table = StripedLockTable::instance();
    const auto& owned = desc_->lockSet();

    for (const auto& entry : desc_->readSet()) {
        const size_t index = lock_table.getStripeIndex(entry.tmvar_addr);
        const bool mine = std::binary_search(owned.begin(), owned.end(), index);
        const uint64_t before = lock_table.loadStateByIndex(index);

        if (StripedLockTable::isLocked(before) && !mine) {
            last_abort_reason_ = AbortReason::ReadLocked;
            return false;
        }
        if (StripedLockTable::version(before) > rv) {
            last_abort_reason_ = AbortReason::ReadVersionChanged;
            return false;
        }
        if (!entry.validator(entry.tmvar_addr, entry.expected_head, rv)) {
            // An update transaction intentionally requires the previously
            // selected node to still be the current head. This is stricter
            // than snapshot isolation but avoids stale-read updates.
            last_abort_reason_ = AbortReason::ReadVersionChanged;
            return false;
        }

        std::atomic_thread_fence(std::memory_order_seq_cst);
        const uint64_t after = lock_table.loadStateByIndex(index);
        if (after != before ||
            (StripedLockTable::isLocked(after) && !mine)) {
            last_abort_reason_ = AbortReason::ReadLocked;
            return false;
        }
    }
    return true;
}

void Transaction::lockWriteSet() {
    auto& locks = desc_->lockSet();
    locks.clear();
    auto& lock_table = StripedLockTable::instance();

    // Build, sort, deduplicate before taking any locks, so bad_alloc
    // cannot strand the transaction holding a partially acquired lockset.
    for (const auto& entry : desc_->writeSet())
        locks.push_back(lock_table.getStripeIndex(entry.tmvar_addr));
    std::sort(locks.begin(), locks.end());
    locks.erase(std::unique(locks.begin(), locks.end()), locks.end());

    for (size_t index : locks)
        lock_table.lockByIndex(index);
}

void Transaction::unlockWriteSet() {
    auto& locks = desc_->lockSet();
    auto& lock_table = StripedLockTable::instance();
    for (auto it = locks.rbegin(); it != locks.rend(); ++it)
        lock_table.unlockByIndex(*it);
    locks.clear();
}

void Transaction::unlockCommittedWriteSet(uint64_t wv) {
    auto& locks = desc_->lockSet();
    auto& lock_table = StripedLockTable::instance();
    for (auto it = locks.rbegin(); it != locks.rend(); ++it)
        lock_table.unlockCommittedByIndex(*it, wv);
    locks.clear();
}

} // namespace Occ
} // namespace STM
