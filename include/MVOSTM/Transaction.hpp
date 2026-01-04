#pragma once

#include "TransactionDescriptor.hpp"
#include "StripedLockTable.hpp"
#include "GlobalClock.hpp"
#include "TMVar.hpp"
#include <algorithm>
#include <cstdint>
#include <vector>

struct RetryException : public std::exception {};


class Transaction {
public:
    explicit Transaction(TransactionDescriptor* desc) : desc_(desc) {}

    void begin();
    bool commit();

    template<typename T>
    T load(TMVar<T>& var);

    template<typename T>
    void store(TMVar<T>& var, const T& val);

private:
    bool validateReadSet();
    void lockWriteSet();
    void unlockWriteSet();

private:
    TransactionDescriptor* desc_;
};


inline void Transaction::begin() {
    desc_->reset();
    desc_->setReadVersion(GlobalClock::now());
}


template<typename T>
T Transaction::load(TMVar<T>& var) {
    using Node = typename TMVar<T>::Node;

    // 1. Read-Your-Own-Writes (总是先查自己有没有写过)
    // 即使是只读模式，如果用户写了代码先 store 再 load，也得能读到（虽然这种变态逻辑很少见）
    auto& wset = desc_->writeSet();
    if (!wset.empty()) {
        for(auto it = wset.rbegin(); it != wset.rend(); ++it) {
            if(it->tmvar_addr == &var) return static_cast<Node*>(it->new_node)->payload;
        }
    }

    // 疑问之处
    if (StripedLockTable::instance().is_locked(&var)) {
        throw RetryException();
    }

    auto* curr = var.loadHead();

    desc_->addToReadSet(&var, TMVar<T>::validate);
    
    uint64_t rv = desc_->getReadVersion();

    // 遍历链表找 <= RV 的版本
    while (curr != nullptr) {
        if (curr->write_ts <= rv) {
            return curr->payload;
        }
        curr = curr->prev;
    }
    
    // 疑问之处
    if (StripedLockTable::instance().is_locked(&var)) {
        throw RetryException();
    }

    // 找不到可见版本
    throw RetryException();    
}

template <typename T>
void Transaction::store(TMVar<T>& var, const T& val) {
    using Node = typename TMVar<T>::Node;
    Node* node = new Node(0, nullptr, val);

    desc_->addToWriteSet(&var, node, TMVar<T>::committer, TMVar<T>::deleter);
}

inline bool Transaction::commit() {
    auto& wset = desc_->writeSet();
    auto& rset = desc_->readSet();

    // 只读事务
    if (desc_->writeSet().empty()) {
        desc_->reset();
        return true;
    }

    lockWriteSet();
    uint64_t wv = GlobalClock::tick();

    if(!validateReadSet()) {
        unlockWriteSet();
        return false;
    }

    for (auto& entry : wset) {
        entry.committer(entry.tmvar_addr, entry.new_node, wv);
        entry.new_node = nullptr;
    }

    unlockWriteSet();

    desc_->reset();
    return true;
}

inline bool Transaction::validateReadSet() {
    uint64_t rv = desc_->getReadVersion();
    auto& lock_table = StripedLockTable::instance();
    auto& locks = desc_->lockSet(); // 我持有的锁

    for(const auto& entry : desc_->readSet()) {
        // 前置锁检查 (Pre-Check)
        if(lock_table.is_locked(entry.tmvar_addr)){
            // 如果被锁了，除非是我自己锁的，否则 Abort
            bool locked_by_me = std::binary_search(locks.begin(), locks.end(), entry.tmvar_addr);
            if(!locked_by_me) return false;
        }

        if(!entry.validator(entry.tmvar_addr, rv)) {
            return false;
        }

        // 强制 CPU 和编译器：必须先完成上面的 STEP 2 (读数据)，
        std::atomic_thread_fence(std::memory_order_seq_cst);

        // 确保读的过程中，没有别的线程偷偷加了锁
        if(lock_table.is_locked(entry.tmvar_addr)){
            bool locked_by_me = std::binary_search(locks.begin(), locks.end(), entry.tmvar_addr);
            if(!locked_by_me) return false;
        }
    }
    return true;
}


inline void Transaction::lockWriteSet() {
    auto& wset = desc_->writeSet();
    auto& locks = desc_->lockSet();
    locks.clear();
    for(auto& entry : wset) locks.push_back(entry.tmvar_addr);

    // 排序去重
    std::sort(locks.begin(), locks.end());
    auto last = std::unique(locks.begin(), locks.end());
    locks.erase(last, locks.end());

    auto& lock_table = StripedLockTable::instance();
    for(void* addr : locks) {
        lock_table.lock(addr);
    }

}


inline void Transaction::unlockWriteSet() {
    auto& locks = desc_->lockSet();
    auto& lock_table = StripedLockTable::instance();
    
    for (void* addr : locks) {
        lock_table.unlock(addr);
    }
}


