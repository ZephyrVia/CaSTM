#pragma once 

#include <cstdint>
#include <utility>
namespace STM {
namespace Ww {

namespace detail {

template<typename T>
struct VersionNode {
    uint64_t write_ts;  // 写入时间戳
    T payload;          // 实际数据

    template<typename... Args>
    VersionNode(uint64_t wts, Args&&... args)
        : write_ts(wts)
        , payload(std::forward<Args>(args)...)
        {}

    VersionNode(const VersionNode&) = delete;
    VersionNode& operator=(const VersionNode&) = delete;

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
