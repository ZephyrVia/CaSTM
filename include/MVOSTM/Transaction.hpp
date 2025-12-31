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


// template <typename T>
// T Transaction::load(TMVar<T>& var) {
//     using Node = typename TMVar<T>::Node;

//     auto& wset = desc_->writeSet();
//     for(auto it = wset.rbegin(); it != wset.rend(); ++it) {
//         if(it->tmvar_addr == &var) {
//             return static_cast<Node*>(it->new_node)->payload;
//         }
//     }

//     auto* curr = var.loadHead();
//     desc_->addToReadSet(&var, TMVar<T>::validate);

//     while (curr != nullptr) {
//         if(curr->write_ts <= desc_->getReadVersion()) {
//             return curr->payload;
//         }
//         curr = curr->prev;
//     }
//     throw RetryException(); 
// }


template<typename T>
T Transaction::load(TMVar<T>& var) {
    using Node = typename TMVar<T>::Node;
    
    // 1. 先查 WriteSet (Read-Your-Own-Writes)
    auto& wset = desc_->writeSet();
    for(auto it = wset.rbegin(); it != wset.rend(); ++it) {
        if(it->tmvar_addr == &var) return static_cast<Node*>(it->new_node)->payload;
    }
    
    // 2. 查 ReadSet (优化：如果已经读过，直接返回之前的结果？)
    // 这一步对于正确性不是必须的，但对于性能有帮助。
    // 为简单起见，这里保持标准逻辑：从内存读取。
    
    auto* curr = var.loadHead();
    desc_->addToReadSet(&var, TMVar<T>::validate);

    // 3. 遍历版本链
    while (curr != nullptr) {
        // 找到可见版本
        if(curr->write_ts <= desc_->getReadVersion()) {
            return curr->payload;
        }
        curr = curr->prev;
    }
    
    // 4. 【关键修复】如果遍历完链表都没找到可见版本 (curr == nullptr)
    // 说明所有版本都比我的 RV 新，或者历史版本已经被剪枝。
    // 必须中止事务！
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

    // 读写事务
    if(!validateReadSet()) {
        return false;
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
    auto& wset = desc_->writeSet();

    for(const auto& entry : desc_->readSet()) {
        if(lock_table.is_locked(entry.tmvar_addr)){
            // 判断是否是自己锁的
            bool locked_by_me = false;
            auto& locks = desc_->lockSet();

            if(std::binary_search(locks.begin(), locks.end(), entry.tmvar_addr)){
                locked_by_me = true;
            }

            if(!locked_by_me) {
                return false;
            }
        }

        if(!entry.validator(entry.tmvar_addr, rv)) {
            return false;
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


