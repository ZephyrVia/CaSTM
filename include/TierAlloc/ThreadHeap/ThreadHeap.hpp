#pragma once

#include "common/SizeClassConfig.hpp"
#include "ThreadHeap/Slab.hpp"
#include "ThreadHeap/SizeClassPool.hpp"
#include "ThreadHeap/ThreadChunkCache.hpp"

class ThreadHeap {
public:
    [[nodiscard]] static void* allocate(size_t nbytes) noexcept;
    static void deallocate(void* ptr) noexcept;

    ThreadHeap(const ThreadHeap&) = delete;
    ThreadHeap& operator=(const ThreadHeap&) = delete;

private:
    ThreadHeap() noexcept;
    ~ThreadHeap() = default;
    static ThreadHeap& local_() noexcept;
    [[nodiscard]] bool isOwnSlab_(const Slab* slab) const noexcept;

private:
    ThreadChunkCache chunk_cache_;
    SizeClassPool pools_[SizeClassConfig::kClassCount];

};