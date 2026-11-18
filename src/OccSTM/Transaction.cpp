#include "OccSTM/Transaction.hpp"
#include "OccSTM/StripedLockTable.hpp"
#include <algorithm>
#include <vector>

namespace STM {
namespace Occ {

// 构造函数
Transaction::Transaction(TransactionDescriptor* desc) : desc_(desc) {}
Transaction::~Transaction() {}


namespace {

// RAII for all stripes acquired by one updating transaction. On failure,
// restore each original version; on success, publish WV at every unlock.
class WriteLockScope {
public:
    explicit WriteLockScope(std::vector<size_t>& indices) noexcept
        : indices_(indices) {}

    ~WriteLockScope() noexcept {
        auto& table = StripedLockTable::instance();
        for (auto it = indices_.rbegin(); it != indices_.rend(); ++it) {
            if (committed_) table.unlockByIndex(*it, write_version_);
            else table.unlockByIndex(*it);
        }
        indices_.clear();
    }

    void markCommitted(uint64_t write_version) noexcept {
        write_version_ = write_version;
        committed_ = true;
    }

private:
    std::vector<size_t>& indices_;
    uint64_t write_version_ = 0;
    bool committed_ = false;
};

} // namespace

bool Transaction::commit() {
    auto& wset = desc_->writeSet();

    // Every load was protected by a versioned read barrier. A fixed RV plus
    // immutable history is a consistent read-only snapshot: no commit-time
    // ReadSet validation is needed in that case.
    if (wset.empty()) {
        desc_->reset();
        return true;
    }

    // lockWriteSet prepares and sorts the index vector *before* acquiring
    // locks; only non-throwing atomic lock operations follow.
    lockWriteSet();
    {
        WriteLockScope locks(desc_->lockSet());

        // TL2: draw a distinct write timestamp *after* acquiring all
        // affected stripes. Disjoint transactions may then publish
        // concurrently and even complete in reverse WV order.
        const uint64_t wv = GlobalClock::tick();
        if (!validateReadSet()) {
            return false;
        }

        // Point of no abort: all potentially throwing draft allocations
        // were performed by store(). Only noexcept publishers run here.
        for (auto& e : wset) {
            e.committer(e.tmvar_addr, e.new_node, wv);
            e.new_node = nullptr;
        }

        // Pruning is optional, non-throwing maintenance. It prepares EBR
        // metadata before severing any history link, while the writer still
        // owns its stripes.
        for (auto& e : wset) {
            if (e.pruner) e.pruner(e.tmvar_addr);
        }

        // Release store of (WV, unlocked) occurs at scope exit, after all
        // heads and their history links have been published.
        locks.markCommitted(wv);
    }

    desc_->commitAllocations();
    desc_->reset();
    return true;
}

bool Transaction::validateReadSet() {
    const uint64_t rv = desc_->getReadVersion();
    auto& table = StripedLockTable::instance();
    const auto& own_stripes = desc_->lockSet(); // sorted size_t indices

    for (const auto& entry : desc_->readSet()) {
        const size_t stripe = table.getStripeIndex(entry.tmvar_addr);
        const bool mine = std::binary_search(
            own_stripes.begin(), own_stripes.end(), stripe);

        const uint64_t before = table.loadStateByIndex(stripe);
        if ((StripedLockTable::isLocked(before) && !mine) ||
            StripedLockTable::version(before) > rv) {
            last_abort_reason_ = AbortReason::ReadLocked;
            return false;
        }

        // Updating transactions use intentionally conservative OCC:
        // historical reads are fine, but a write based on any non-head
        // value (or a head already changed) is rejected.
        if (!entry.validator(entry.tmvar_addr, entry.expected_head, rv)) {
            last_abort_reason_ = AbortReason::ReadVersionChanged;
            return false;
        }

        std::atomic_thread_fence(std::memory_order_acq_rel);
        const uint64_t after = table.loadStateByIndex(stripe);
        if (before != after || (StripedLockTable::isLocked(after) && !mine)) {
            last_abort_reason_ = AbortReason::ReadLocked;
            return false;
        }
    }
    return true;
}


void Transaction::lockWriteSet() {
    auto& stripes = desc_->lockSet();
    stripes.clear();

    auto& table = StripedLockTable::instance();
    // All potentially allocating operations happen before acquiring locks.
    for (const auto& entry : desc_->writeSet()) {
        stripes.push_back(table.getStripeIndex(entry.tmvar_addr));
    }
    std::sort(stripes.begin(), stripes.end());
    stripes.erase(std::unique(stripes.begin(), stripes.end()), stripes.end());

    // Ascending order prevents deadlock among multi-stripe writers.
    for (const size_t stripe : stripes) {
        table.lockByIndex(stripe);
    }
}

} // namespace Occ
} // namespace STM
