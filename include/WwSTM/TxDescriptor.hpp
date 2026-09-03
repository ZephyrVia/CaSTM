#pragma once 

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <new>

#include "Config.hpp"
#include "TxStatus.hpp"
#include "TierAlloc/common/GlobalConfig.hpp"
namespace STM {
namespace Ww {

struct alignas(kCacheLineSize) TxDescriptor  {
    std::atomic<TxStatus> status;

    const uint64_t start_ts;

#if STM_WW_TEST_HOOKS || STM_WW_VERIFY_LOGIC_MODE
    inline static std::atomic<uint64_t> next_debug_tx_id_{1};
    const uint64_t debug_tx_id;
#endif

    // Commit B uses this per-descriptor gate to serialize owner writes with
    // the prepare phase.  The descriptor itself is retired only after all
    // published records for the transaction have been removed.
    mutable std::mutex write_gate;
    std::atomic<bool> prepare_started;

    explicit TxDescriptor(uint64_t ts) 
        : status(TxStatus::ACTIVE)
        , start_ts(ts)
#if STM_WW_TEST_HOOKS || STM_WW_VERIFY_LOGIC_MODE
        , debug_tx_id(next_debug_tx_id_.fetch_add(1, std::memory_order_relaxed))
#endif
        , prepare_started(false)
    {}

    bool writePhaseOpen() const noexcept {
        return status.load(std::memory_order_acquire) == TxStatus::ACTIVE
            && !prepare_started.load(std::memory_order_acquire);
    }

    virtual ~TxDescriptor() {
#if STM_WW_TEST_HOOKS
        debug_destroyed_count.fetch_add(1, std::memory_order_relaxed);
#endif
    }

    // 禁止拷贝和移动：Descriptor 是具有唯一地址标识的实体
    TxDescriptor(const TxDescriptor&) = delete;
    TxDescriptor& operator=(const TxDescriptor&) = delete;
    TxDescriptor(TxDescriptor&&) = delete;
    TxDescriptor& operator=(TxDescriptor&&) = delete;

    static void* operator new(std::size_t size) {
        void* ptr = ::operator new(size, std::align_val_t(alignof(TxDescriptor)));
#if STM_WW_TEST_HOOKS
        debug_allocated_count.fetch_add(1, std::memory_order_relaxed);
#endif
        return ptr;
    }

    static void* operator new(std::size_t size, std::align_val_t alignment) {
        void* ptr = ::operator new(size, alignment);
#if STM_WW_TEST_HOOKS
        debug_allocated_count.fetch_add(1, std::memory_order_relaxed);
#endif
        return ptr;
    }

    static void operator delete(void* p) noexcept {
#if STM_WW_TEST_HOOKS
        debug_reclaimed_count.fetch_add(1, std::memory_order_relaxed);
#endif
        ::operator delete(p, std::align_val_t(alignof(TxDescriptor)));
    }

    static void operator delete(void* p, std::size_t) noexcept {
        operator delete(p);
    }

    static void operator delete(void* p, std::align_val_t alignment) noexcept {
#if STM_WW_TEST_HOOKS
        debug_reclaimed_count.fetch_add(1, std::memory_order_relaxed);
#endif
        ::operator delete(p, alignment);
    }

    static void operator delete(void* p,
                                std::size_t,
                                std::align_val_t alignment) noexcept {
        operator delete(p, alignment);
    }

#if STM_WW_TEST_HOOKS
    inline static std::atomic<uint64_t> debug_allocated_count{0};
    inline static std::atomic<uint64_t> debug_reclaimed_count{0};
    inline static std::atomic<uint64_t> debug_destroyed_count{0};

    static void resetDebugLifetimeCounters() noexcept {
        debug_allocated_count.store(0, std::memory_order_relaxed);
        debug_reclaimed_count.store(0, std::memory_order_relaxed);
        debug_destroyed_count.store(0, std::memory_order_relaxed);
    }

    static uint64_t debugAllocatedCount() noexcept {
        return debug_allocated_count.load(std::memory_order_relaxed);
    }

    static uint64_t debugReclaimedCount() noexcept {
        return debug_reclaimed_count.load(std::memory_order_relaxed);
    }

    static uint64_t debugDestroyedCount() noexcept {
        return debug_destroyed_count.load(std::memory_order_relaxed);
    }
#endif

};

}
}
