#pragma once

#include "TransactionDescriptor.hpp"
#include "StripedLockTable.hpp"
#include "GlobalClock.hpp"
#include "TMVar.hpp"
#include <algorithm>
#include <cstdint>
#include <vector>
#include <memory>

namespace STM {
namespace Occ {

struct RetryException : public std::exception {};

// Transaction-local diagnostic state. No global counters or timing on the
// transaction path; callers can inspect the most recent failed attempt.
enum class AbortReason {
    None,
    SnapshotUnavailable,
    ReadLocked,
    ReadVersionChanged
};

class Transaction {
public:
    explicit Transaction(TransactionDescriptor* desc);
    ~Transaction();

    void begin() {
        desc_->reset();
        last_abort_reason_ = AbortReason::None;
        desc_->setReadVersion(GlobalClock::now());
    }

    bool commit();
    AbortReason lastAbortReason() const noexcept { return last_abort_reason_; }

    template<typename T>
    T load(TMVar<T>& var);

    template<typename T>
    void store(TMVar<T>& var, const T& val);

    template<typename T, typename... Args>
    T* alloc(Args&&... args);

    template<typename T>
    void free(T* ptr);

private:
    bool validateReadSet();
    void lockWriteSet();

private:
    TransactionDescriptor* desc_;
    AbortReason last_abort_reason_ = AbortReason::None;
};


template<typename T>
T Transaction::load(TMVar<T>& var) {
    using Node = typename TMVar<T>::Node;

    // 1. Read-Your-Own-Writes
    auto& wset = desc_->writeSet();
    if (!wset.empty()) {
        for(auto it = wset.rbegin(); it != wset.rend(); ++it) {
            if(it->tmvar_addr == &var) return static_cast<Node*>(it->new_node)->payload;
        }
    }

    auto& locks = StripedLockTable::instance();

    // TL2 read barrier around an immutable MVCC chain traversal. Unlike
    // single-version TL2, a newer unlocked stripe version is allowed:
    // selecting the latest VersionNode with ts <= RV gives the old snapshot.
    const uint64_t before = locks.loadState(&var);
    if (StripedLockTable::isLocked(before)) {
        last_abort_reason_ = AbortReason::ReadLocked;
        throw RetryException();
    }

    Node* curr = var.loadHead();
    const uint64_t rv = desc_->getReadVersion();
    while (curr && curr->write_ts > rv) {
        // Other writers can sever a history edge under their stripe lock.
        // EBR protects any node pointer already obtained.
        curr = curr->prev.load(std::memory_order_acquire);
    }
    if (!curr) {
        last_abort_reason_ = AbortReason::SnapshotUnavailable;
        throw RetryException();
    }

    T value = curr->payload;

    // Prevent the data read from being reordered after the post-check.
    std::atomic_thread_fence(std::memory_order_acq_rel);
    const uint64_t after = locks.loadState(&var);
    if (StripedLockTable::isLocked(after) || before != after) {
        last_abort_reason_ = AbortReason::ReadVersionChanged;
        throw RetryException();
    }

    // For an updating transaction, commit will conservatively require
    // this selected version to *still be the head* (strict OCC validation).
    desc_->addToReadSet(&var, curr, TMVar<T>::validate);
    return value;
}

template <typename T>
void Transaction::store(TMVar<T>& var, const T& val) {
    using Node = typename TMVar<T>::Node;
    Node* node = new Node(0, nullptr, val);

    try {
        desc_->addToWriteSet(&var, node, TMVar<T>::committer,
                             TMVar<T>::pruneNoThrow, TMVar<T>::deleter);
    } catch (...) {
        delete node; // A failed WriteSet allocation must not leak its draft.
        throw;
    }
}


template<typename T, typename... Args>
T* Transaction::alloc(Args&&... args) {
    // 使用线程堆分配内存，并做记录
    void* raw_mem = STM::Memory::occAllocate(sizeof(T));
    desc_->recordAllocation(raw_mem);
    return new(raw_mem) T(std::forward<Args>(args)...);
}


template<typename T>
void Transaction::free(T* ptr) {
    if (!ptr) return;

    ptr->~T();
    STM::Memory::occDeallocate(ptr);
}

} // namespace Occ
} // namespace STM
