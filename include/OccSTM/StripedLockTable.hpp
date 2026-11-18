#pragma once
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <immintrin.h>
#include <thread>

namespace STM {
namespace Occ {

// A TL2-style (committed version, lock bit) word per stripe. Writers hold
// ALL their stripes until ALL new heads have been published. This prevents
// readers from accepting a partially published multi-variable transaction.
class StripedLockTable {
public:
    static constexpr size_t kTableSize = 1 << 20;
    static constexpr size_t kTableMask = kTableSize - 1;
    static constexpr uint64_t kLockedBit = 1;

    static StripedLockTable& instance() noexcept {
        static StripedLockTable table;
        return table;
    }

    size_t getStripeIndex(const void* addr) const noexcept {
        return std::hash<const void*>{}(addr) & kTableMask;
    }

    static bool isLocked(uint64_t state) noexcept {
        return (state & kLockedBit) != 0;
    }
    static uint64_t version(uint64_t state) noexcept {
        return state >> 1;
    }
    static uint64_t pack(uint64_t version, bool locked) noexcept {
        return (version << 1) | static_cast<uint64_t>(locked);
    }

    uint64_t loadStateByIndex(size_t index) const noexcept {
        return locks_[index].state.load(std::memory_order_seq_cst);
    }

    // TTAS: acquiring a write lock does not change its committed version.
    void lockByIndex(size_t index) noexcept {
        auto& state = locks_[index].state;
        for (;;) {
            uint64_t old = state.load(std::memory_order_seq_cst);
            if (isLocked(old)) {
                _mm_pause();
                continue;
            }
            if (state.compare_exchange_weak(
                    old, old | kLockedBit,
                    std::memory_order_seq_cst,
                    std::memory_order_seq_cst)) {
                return;
            }
            std::this_thread::yield();
        }
    }

    // Abort/manual test unlock: preserve the previous committed version.
    void unlockByIndex(size_t index) noexcept {
        auto& state = locks_[index].state;
        const uint64_t old = state.load(std::memory_order_relaxed);
        assert(isLocked(old));
        state.store(old & ~kLockedBit, std::memory_order_seq_cst);
    }

    // Commit unlock: publication is complete for EVERY written TMVar.
    void unlockCommittedByIndex(size_t index, uint64_t wv) noexcept {
        assert(isLocked(locks_[index].state.load(std::memory_order_relaxed)));
        locks_[index].state.store(pack(wv, false),
                                  std::memory_order_seq_cst);
    }

    void lock(const void* addr) noexcept { lockByIndex(getStripeIndex(addr)); }
    void unlock(const void* addr) noexcept { unlockByIndex(getStripeIndex(addr)); }

    bool is_locked(const void* addr) const noexcept {
        return isLocked(loadStateByIndex(getStripeIndex(addr)));
    }

private:
    struct alignas(64) LockEntry {
        std::atomic<uint64_t> state{0};
    };

    StripedLockTable() { locks_ = new LockEntry[kTableSize]; }
    ~StripedLockTable() { delete[] locks_; }

    LockEntry* locks_ = nullptr;
};

} // namespace Occ
} // namespace STM
