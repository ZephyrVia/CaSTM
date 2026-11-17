#include "ThreadHeap/ThreadHeapDepot.hpp"
#include "ThreadHeap/ThreadHeap.hpp"

#include <cassert>
#include <new>

ThreadHeapDepot& ThreadHeapDepot::GetInstance() noexcept {
    // Do not destruct managers while EBR callbacks or published objects may
    // still refer to their Slabs at process shutdown.
    static auto* instance = new ThreadHeapDepot();
    return *instance;
}

ThreadHeap* ThreadHeapDepot::acquire() noexcept {
    // The mutex transfers the exclusive right to mutate local Slab/pool state.
    std::lock_guard<std::mutex> lock(mutex_);

    if (idle_head_) {
        ThreadHeap* heap = idle_head_;
        assert(heap->depot_idle_);
        idle_head_ = heap->depot_next_idle_;
        heap->depot_next_idle_ = nullptr;
        heap->depot_idle_ = false;

        assert(idle_heaps_ > 0);
        --idle_heaps_;
        ++leased_heaps_;
        ++reuse_events_;
        return heap;
    }

    // Construct a new manager only at the high-water mark of concurrency.
    ThreadHeap* heap = new (std::nothrow) ThreadHeap();
    if (heap) {
        ++created_heaps_;
        ++leased_heaps_;
    }
    return heap;
}

void ThreadHeapDepot::park(ThreadHeap* heap) noexcept {
    if (!heap) return;

    // Park has no dynamic allocations and never returns Slabs to CentralHeap.
    std::lock_guard<std::mutex> lock(mutex_);
    assert(!heap->depot_idle_);
    assert(heap->depot_next_idle_ == nullptr);
    assert(leased_heaps_ > 0);

    heap->depot_idle_ = true;
    heap->depot_next_idle_ = idle_head_;
    idle_head_ = heap;
    --leased_heaps_;
    ++idle_heaps_;
}

ThreadHeapDepotStats ThreadHeapDepot::stats() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return {created_heaps_, leased_heaps_, idle_heaps_, reuse_events_};
}

ThreadHeapLease::ThreadHeapLease() noexcept
    : heap_(ThreadHeapDepot::GetInstance().acquire()) {}

ThreadHeapLease::~ThreadHeapLease() noexcept {
    ThreadHeapDepot::GetInstance().park(heap_);
    heap_ = nullptr;
}
