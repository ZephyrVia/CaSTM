#pragma once 

#include <atomic>
#include <cstdint>
#include <utility>
namespace STM {
namespace Ww {

namespace detail {

template<typename T>
struct VersionNode {
    // Commit B assigns the final timestamp during preparation, before the
    // transaction descriptor becomes COMMITTED.  Atomic access keeps reads
    // well-defined while helpers observe and flatten a published record.
    std::atomic<uint64_t> write_ts;  // 写入时间戳
    T payload;          // 实际数据

    template<typename... Args>
    VersionNode(uint64_t wts, Args&&... args)
        : write_ts(wts)
        , payload(std::forward<Args>(args)...)
        {}

    VersionNode(const VersionNode&) = delete;
    VersionNode& operator=(const VersionNode&) = delete;

    uint64_t loadWriteTs() const noexcept {
        return write_ts.load(std::memory_order_acquire);
    }

    void storeWriteTs(uint64_t ts) noexcept {
        write_ts.store(ts, std::memory_order_release);
    }

    static void* operator new(size_t size) {
        return ::operator new(size);
    }

    static void operator delete(void* p) {
        ::operator delete(p);
    }

};

}

}
}
