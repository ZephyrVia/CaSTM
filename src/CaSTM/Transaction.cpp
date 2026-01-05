#include "CaSTM/Transaction.hpp"
#include "CaSTM/StripedLockTable.hpp"
#include <algorithm>
#include <vector>

// 构造函数
Transaction::Transaction(TransactionDescriptor* desc) : desc_(desc) {}
Transaction::~Transaction() {}


bool Transaction::commit() {
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

    desc_->commitAllocations(); 
    desc_->reset();
    return true;
}

bool Transaction::validateReadSet() {
    uint64_t rv = desc_->getReadVersion();
    auto& lock_table = StripedLockTable::instance();
    auto& locks = desc_->lockSet(); // 这里面存的是 (void*)index

    for(const auto& entry : desc_->readSet()) {
        // 前置锁检查
        if(lock_table.is_locked(entry.tmvar_addr)){
            // 必须先计算出地址对应的索引
            size_t idx = lock_table.getStripeIndex(entry.tmvar_addr);
            void* idx_ptr = reinterpret_cast<void*>(idx);

            // 然后在 locks 列表中查找这个索引
            bool locked_by_me = std::binary_search(locks.begin(), locks.end(), idx_ptr);
            
            // 如果被锁了且不是我锁的 -> 冲突
            if(!locked_by_me) return false;
        }

        // 身份 + 时间验证
        if(!entry.validator(entry.tmvar_addr, entry.expected_head, rv)) {
            return false;
        }

        // lfence：防止后面的锁检查被排到前面
        std::atomic_thread_fence(std::memory_order_acquire);

        if(lock_table.is_locked(entry.tmvar_addr)){
            size_t idx = lock_table.getStripeIndex(entry.tmvar_addr);
            void* idx_ptr = reinterpret_cast<void*>(idx);

            bool locked_by_me = std::binary_search(locks.begin(), locks.end(), idx_ptr);
            if(!locked_by_me) return false;
        }
    }
    return true;
}


void Transaction::lockWriteSet() {
    auto& wset = desc_->writeSet();
    auto& locks = desc_->lockSet();
    
    locks.clear();
    
    auto& lock_table = StripedLockTable::instance();
    
    // 1. 直接填入 locks
    for(auto& entry : wset) {
        size_t idx = lock_table.getStripeIndex(entry.tmvar_addr);
        locks.push_back(reinterpret_cast<void*>(idx));
    }

    // 2. 在 locks 上排序去重
    std::sort(locks.begin(), locks.end());
    auto last = std::unique(locks.begin(), locks.end());
    locks.erase(last, locks.end());

    // 3. 遍历加锁
    for(void* ptr_idx : locks) {
        size_t idx = reinterpret_cast<size_t>(ptr_idx);
        lock_table.lockByIndex(idx);
    }
}

void Transaction::unlockWriteSet() {
    auto& locks = desc_->lockSet();
    auto& lock_table = StripedLockTable::instance();
    
    for (auto it = locks.rbegin(); it != locks.rend(); ++it) {
        size_t idx = reinterpret_cast<size_t>(*it);
        lock_table.unlockByIndex(idx);
    }
    locks.clear();
}
