#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <thread>
#include <immintrin.h>

// TL2-style versioned per-stripe write locks. A single atomic word encodes
// the last committed write version (upper bits) and the locked bit (bit 0).
// The version changes only when a transaction commits a write to this stripe.
namespace STM {
namespace Occ {

class StripedLockTable {
public:
    static constexpr size_t kTableSize = 1 << 20;
    static constexpr size_t kTableMask = kTableSize - 1;
    static constexpr uint64_t kLockBit = 1;

    static StripedLockTable& instance() noexcept {
        static StripedLockTable table;
        return table;
    }

    static bool isLocked(uint64_t state) noexcept {
        return (state & kLockBit) != 0;
    }

    static uint64_t version(uint64_t state) noexcept {
        return state >> 1;
    }

    static uint64_t unlockedState(uint64_t version) noexcept {
        return version << 1;
    }

    size_t getStripeIndex(const void* addr) const noexcept {
        return std::hash<const void*>{}(addr) & kTableMask;
    }

    uint64_t loadStateByIndex(size_t index) const noexcept {
        return locks_[index].state.load(std::memory_order_acquire);
    }

    uint64_t loadState(const void* addr) const noexcept {
        return loadStateByIndex(getStripeIndex(addr));
    }

    // TTAS + CAS. Lock acquisition preserves the last committed version.
    // Writers must obtain *all* write stripes before GlobalClock::tick().
    void lockByIndex(size_t index) noexcept {
        auto& word = locks_[index].state;
        for (;;) {
            uint64_t old = word.load(std::memory_order_relaxed);
            if (isLocked(old)) {
                _mm_pause();
                continue;
            }
            if (word.compare_exchange_weak(
                    old, old | kLockBit,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed)) {
                return;
            }
            std::this_thread::yield();
        }
    }

    // Failed/aborted update, or explicit/manual lock: no new version.
    void unlockByIndex(size_t index) noexcept {
        auto& word = locks_[index].state;
        const uint64_t locked = word.load(std::memory_order_relaxed);
        word.store(locked & ~kLockBit, std::memory_order_release);
    }

    // Committed update: release with a new version after *all* of this
    // transaction's TMVar heads have been published.
    void unlockByIndex(size_t index, uint64_t committed_version) noexcept {
        locks_[index].state.store(
            unlockedState(committed_version), std::memory_order_release);
    }

    void lock(const void* addr) noexcept {
        lockByIndex(getStripeIndex(addr));
    }

    void unlock(const void* addr) noexcept {
        unlockByIndex(getStripeIndex(addr));
    }

    bool is_locked(const void* addr) const noexcept {
        return isLocked(loadState(addr));
    }

private:
    struct alignas(64) LockEntry {
        std::atomic<uint64_t> state{0};
    };

    StripedLockTable() {
        locks_ = new LockEntry[kTableSize];
    }

    ~StripedLockTable() {
        delete[] locks_;
    }

    LockEntry* locks_ = nullptr;
};

} // namespace Occ
} // namespace STM
