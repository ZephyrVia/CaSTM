#pragma once 

#include <atomic>
#include <cstdint>
#include <mutex>

#include "TxStatus.hpp"
#include "TierAlloc/common/GlobalConfig.hpp"
namespace STM {
namespace Ww {

struct alignas(kCacheLineSize) TxDescriptor  {
    std::atomic<TxStatus> status;

    const uint64_t start_ts;

    // Commit B uses this per-descriptor gate to serialize owner writes with
    // the prepare phase.  It does not change descriptor reclamation: the
    // descriptor remains intentionally unreclaimed for now.
    mutable std::mutex write_gate;
    std::atomic<bool> prepare_started;

    explicit TxDescriptor(uint64_t ts) 
        : status(TxStatus::ACTIVE)
        , start_ts(ts)
        , prepare_started(false)
    {}

    bool writePhaseOpen() const noexcept {
        return status.load(std::memory_order_acquire) == TxStatus::ACTIVE
            && !prepare_started.load(std::memory_order_acquire);
    }

    virtual ~TxDescriptor() = default;

    // 禁止拷贝和移动：Descriptor 是具有唯一地址标识的实体
    TxDescriptor(const TxDescriptor&) = delete;
    TxDescriptor& operator=(const TxDescriptor&) = delete;
    TxDescriptor(TxDescriptor&&) = delete;
    TxDescriptor& operator=(TxDescriptor&&) = delete;

    static void* operator new(size_t size) {
        return ::operator new(size);
    }

    static void operator delete(void* p) {
        ::operator delete(p);
    }

};

}
}
