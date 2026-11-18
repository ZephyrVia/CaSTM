#pragma once

#include "TransactionDescriptor.hpp"
#include "StripedLockTable.hpp"
#include "GlobalClock.hpp"
#include "TMVar.hpp"
#include <algorithm>
#include <cstdint>
#include <vector>

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
    void unlockWriteSet();
    void unlockCommittedWriteSet(uint64_t write_version);

private:
    TransactionDescriptor* desc_;
    AbortReason last_abort_reason_ = AbortReason::None;
};


template<typename T>
T Transaction::load(TMVar<T>& var) {
    using Node = typename TMVar<T>::Node;

    // Reads of our own unpublished drafts need no shared memory barrier.
    auto& wset = desc_->writeSet();
    for (auto it = wset.rbegin(); it != wset.rend(); ++it) {
        if (it->tmvar_addr == &var)
            return static_cast<Node*>(it->new_node)->payload;
    }

    auto& locks = StripedLockTable::instance();
    const size_t stripe = locks.getStripeIndex(&var);
    const uint64_t before = locks.loadStateByIndex(stripe);
    if (StripedLockTable::isLocked(before)) {
        last_abort_reason_ = AbortReason::ReadLocked;
        throw RetryException();
    }

    Node* curr = var.loadHead();
    const uint64_t rv = desc_->getReadVersion();
    while (curr && curr->write_ts > rv)
        curr = curr->prev.load(std::memory_order_acquire);
    if (!curr) {
        last_abort_reason_ = AbortReason::SnapshotUnavailable;
        throw RetryException();
    }

    // Under EBR, the immutable payload is safe to copy. The post-read
    // barrier rejects any concurrent commit affecting this stripe.
    T value = curr->payload;
    std::atomic_thread_fence(std::memory_order_seq_cst);
    const uint64_t after = locks.loadStateByIndex(stripe);
    if (before != after || StripedLockTable::isLocked(after)) {
        last_abort_reason_ = AbortReason::ReadLocked;
        throw RetryException();
    }

    // Unlike pure TL2, historical reads (selected.write_ts <= RV) are legal.
    // Update transactions revalidate dependencies more strictly at commit.
    desc_->addToReadSet(&var, curr, TMVar<T>::validate);
    return value;
}

template <typename T>
void Transaction::store(TMVar<T>& var, const T& val) {
    using Node = typename TMVar<T>::Node;
    Node* node = new Node(0, nullptr, val);

    desc_->addToWriteSet(&var, node, TMVar<T>::committer,
                         TMVar<T>::deleter, TMVar<T>::pruneNoThrow);
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
