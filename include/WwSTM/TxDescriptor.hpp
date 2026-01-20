#pragma once 

#include <atomic>
#include <cstdint>

#include "TxStatus.hpp"
#include "TierAlloc/ThreadHeap/ThreadHeap.hpp"

namespace STM {
namespace Ww {

struct alignas(kCacheLineSize) TxDescriptor  {
    std::atomic<TxStatus> status;

    const uint64_t start_ts;

    explicit TxDescriptor(uint64_t ts) 
        : status(TxStatus::ACTIVE)
        , start_ts(ts)
    {}

    virtual ~TxDescriptor() = default;

    // 禁止拷贝和移动：Descriptor 是具有唯一地址标识的实体
    TxDescriptor(const TxDescriptor&) = delete;
    TxDescriptor& operator=(const TxDescriptor&) = delete;
    TxDescriptor(TxDescriptor&&) = delete;
    TxDescriptor& operator=(TxDescriptor&&) = delete;

    static void* operator new(size_t size) {
        return ThreadHeap::allocate(size);
    }

    static void operator delete(void* p) {
        ThreadHeap::deallocate(p);
    }

};

}
}