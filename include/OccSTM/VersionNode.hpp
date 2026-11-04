#pragma once
#include <atomic>
#include "STM/Allocator.hpp"

namespace STM {
namespace Occ {

// 使用 detail 命名空间，告诉用户：这里的代码不要乱动
namespace detail {

template<typename T>
struct VersionNode {
    uint64_t write_ts;
    // Writers can detach history after this node has been published. EBR
    // protects node lifetime; atomic access separately protects this link.
    std::atomic<VersionNode*> prev;
    T payload;

    template<typename... Args>
    VersionNode(uint64_t v, VersionNode* p, Args&&... args) 
        : write_ts(v)
        , prev(p)
        , payload(std::forward<Args>(args)...)
        {}

    static void* operator new(size_t size) { return STM::Memory::occAllocate(size); }
    static void operator delete(void *p) { STM::Memory::occDeallocate(p); }
};

} // namespace detail

} // namespace Occ
} // namespace STM
