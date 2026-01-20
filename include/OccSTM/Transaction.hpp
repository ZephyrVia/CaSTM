#pragma once

#include "TransactionDescriptor.hpp"
#include "StripedLockTable.hpp"
#include "GlobalClock.hpp"
#include "TMVar.hpp"
#include <algorithm>
#include <cstdint>
#include <vector>
#include <atomic> 

namespace STM {
namespace Occ {

struct RetryException : public std::exception {};


class Transaction {
public:
    explicit Transaction(TransactionDescriptor* desc);
    ~Transaction();

    void begin() {
        desc_->reset();
        desc_->setReadVersion(GlobalClock::now());
    }

    bool commit();

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

private:
    TransactionDescriptor* desc_;
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

    auto* curr = var.loadHead();

        
    uint64_t rv = desc_->getReadVersion();

    // 遍历链表找 <= RV 的版本
    while (curr != nullptr && curr->write_ts > rv) {
        curr = curr->prev;
    }
    
    if(curr == nullptr) {
        // TODO：可更改为return nullptr
        throw RetryException();   
    }

    desc_->addToReadSet(&var, curr, TMVar<T>::validate);

    return curr->payload;
}

template <typename T>
void Transaction::store(TMVar<T>& var, const T& val) {
    using Node = typename TMVar<T>::Node;
    Node* node = new Node(0, nullptr, val);

    desc_->addToWriteSet(&var, node, TMVar<T>::committer, TMVar<T>::deleter);
}


template<typename T, typename... Args>
T* Transaction::alloc(Args&&... args) {
    // 使用线程堆分配内存，并做记录
    void* raw_mem = ThreadHeap::allocate(sizeof(T));
    desc_->recordAllocation(raw_mem);
    return new(raw_mem) T(std::forward<Args>(args)...);
}


template<typename T>
void Transaction::free(T* ptr) {
    if (!ptr) return;

    ptr->~T();
    ThreadHeap::deallocate(ptr);
}

} // namespace Occ
} // namespace STM