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

    // 创建序号：start_ts 相同（同一次 tick 内构造）时的确定性仲裁依据。
    // 不能用堆地址比较——地址顺序取决于分配历史，会让 Wound-Wait 的
    // 新老裁决随测试运行顺序随机翻转（见 resolveConflict 平局分支）。
    const uint64_t creation_serial;

    explicit TxDescriptor(uint64_t ts) 
        : status(TxStatus::ACTIVE)
        , start_ts(ts)
        , creation_serial(next_serial_.fetch_add(1, std::memory_order_relaxed))
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

private:
    inline static std::atomic<uint64_t> next_serial_{1};

};

}
}