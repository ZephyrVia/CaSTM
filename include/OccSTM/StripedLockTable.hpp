#pragma once

#include <atomic>
#include <thread>
#include <functional>
#include <immintrin.h> 

namespace STM {
namespace Occ {

class StripedLockTable {
public:
    static constexpr size_t kTableSize = 1 << 20; 
    static constexpr size_t kTableMask = kTableSize - 1; 

    static StripedLockTable& instance() noexcept {
        static StripedLockTable table;
        return table;
    } 

    size_t getStripeIndex(const void* addr) const noexcept {
        return std::hash<const void*>{}(addr) & kTableMask;
    }

    // 非递归自旋锁 (TTAS)
    // 注意：调用者需确保 index 不重复以避免死锁
    void lockByIndex(size_t index) noexcept {
        LockEntry& entry = locks_[index];
        
        while (true) {
            // 1. Test (Read): 减少缓存颠簸
            if (entry.flag.load(std::memory_order_relaxed)) {
                _mm_pause();
                continue;
            }

            // 2. Set (Write): 尝试获取锁
            if (!entry.flag.exchange(true, std::memory_order_acquire)) {
                return;
            }
            
            std::this_thread::yield(); 
        }
    }

    void unlockByIndex(size_t index) noexcept {
        locks_[index].flag.store(false, std::memory_order_release);
    }

    // 原始地址操作接口
    void lock(const void* addr) noexcept { lockByIndex(getStripeIndex(addr)); }
    void unlock(const void* addr) noexcept { unlockByIndex(getStripeIndex(addr)); }

    bool is_locked(const void* addr) const noexcept {
        return locks_[getStripeIndex(addr)].flag.load(std::memory_order_acquire);
    }

private:
    // 64字节对齐，防止伪共享 (False Sharing)
    struct alignas(64) LockEntry {
        std::atomic<bool> flag{false};
    };

    StripedLockTable() {
        locks_ = new LockEntry[kTableSize];
        for (size_t i = 0; i < kTableSize; ++i) {
            locks_[i].flag.store(false, std::memory_order_relaxed);
        }
    }

    ~StripedLockTable() { 
        delete[] locks_; 
    }

    LockEntry* locks_ = nullptr;
};

}
}