#pragma once 

#include <atomic>
#include <cstdint>
#include <utility>
namespace STM {
namespace Ww {

namespace detail {

template<typename T>
struct VersionNode {
    // Commit A keeps the legacy compatibility path, where the final commit
    // timestamp is assigned during post-COMMITTED cleanup.  Atomic access
    // prevents that temporary ordering from becoming a C++ data race;
    // Commit B will move the store into its prepare phase.
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
