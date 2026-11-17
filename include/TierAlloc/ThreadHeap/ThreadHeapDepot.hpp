#pragma once

#include <cstddef>
#include <mutex>

class ThreadHeap;

struct ThreadHeapDepotStats {
    std::size_t created_heaps = 0;
    std::size_t leased_heaps = 0;
    std::size_t idle_heaps = 0;
    std::size_t reuse_events = 0;
};

// Process-lifetime holder of stable ThreadHeap instances.
// A parked heap may contain live Slab objects and pending remote frees.
class ThreadHeapDepot {
public:
    static ThreadHeapDepot& GetInstance() noexcept;
    [[nodiscard]] ThreadHeapDepotStats stats() const noexcept;

    ThreadHeapDepot(const ThreadHeapDepot&) = delete;
    ThreadHeapDepot& operator=(const ThreadHeapDepot&) = delete;

private:
    friend class ThreadHeapLease;

    ThreadHeapDepot() = default;
    ~ThreadHeapDepot() = default;

    [[nodiscard]] ThreadHeap* acquire() noexcept;
    void park(ThreadHeap* heap) noexcept;

    mutable std::mutex mutex_;
    ThreadHeap* idle_head_ = nullptr;
    std::size_t created_heaps_ = 0;
    std::size_t leased_heaps_ = 0;
    std::size_t idle_heaps_ = 0;
    std::size_t reuse_events_ = 0;
};

// A thread-local RAII lease, not the storage owner.
// The actual ThreadHeap and its SizeClassPools survive TLS destruction.
class ThreadHeapLease {
public:
    ThreadHeapLease() noexcept;
    ~ThreadHeapLease() noexcept;

    ThreadHeapLease(const ThreadHeapLease&) = delete;
    ThreadHeapLease& operator=(const ThreadHeapLease&) = delete;

    [[nodiscard]] ThreadHeap* get() const noexcept { return heap_; }

private:
    ThreadHeap* heap_ = nullptr;
};
