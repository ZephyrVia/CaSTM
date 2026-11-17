#include "EBRManager/ThreadSlotManager.hpp"
#include <utility>
#include <new>      // for std::nothrow
#include <mutex>
#include <cstdio>
#include <cstdlib>

ThreadSlotManager::ThreadSlotManager()
    : capacity_(0) {}

ThreadSlotManager::~ThreadSlotManager() {}


ThreadSlot* ThreadSlotManager::getLocalSlot() {

    thread_local LocalSlotProxy g_local_slot_proxy;

    if(!g_local_slot_proxy.hasSlot()) {
        ThreadSlot* new_slot = acquireSlot_();
        if(!new_slot) {
            return nullptr;
        }
        g_local_slot_proxy.acquire(this, new_slot);
    }

    return g_local_slot_proxy.get();
}

void ThreadSlotManager::releaseSlot_(ThreadSlot* slot) noexcept {
    if (!slot) return;

    // A thread may return its slot only after all EBR critical sections
    // have ended. Recycling an active announcement would let a new thread
    // replace the epoch protecting the former owner's outstanding pointers.
    // Do not silently clear an active slot or make it available for reuse.
    if (ThreadSlot::isActive(slot->loadState())) {
        std::fputs(
            "EBR ThreadSlotManager: active ThreadSlot at thread exit "
            "(unbalanced enter/leave)\n",
            stderr);
        std::abort();
    }

    // End the old thread's registration before publishing the slot to the
    // reuse stack. The next owner will register a fresh epoch in enter().
    slot->unregister();
    free_slots_.push(slot);
}

ThreadSlot* ThreadSlotManager::acquireSlot_() {

    ThreadSlot* slot = free_slots_.pop();
    if(slot) {
        return slot;
    }

    return expandAndAcquire();
}

ThreadSlot* ThreadSlotManager::expandAndAcquire() {
    // 使用 std::mutex 对应的锁保护
    std::lock_guard<std::mutex> lock(resize_lock_);

    // Double Check: 再次检查空闲链表
    ThreadSlot* slot = free_slots_.pop();
    if(slot) {
        return slot;
    }

    const size_t current_capacity = capacity_.load(std::memory_order_relaxed);
    const size_t new_slots_to_add = (current_capacity == 0) ? kInitialCapacity : current_capacity;

    // 1. 使用系统堆分配内存 (System Heap Allocation)
    // 使用 std::nothrow 以便在失败时返回 nullptr，而不是抛出异常
    ThreadSlot* new_slots_array = new (std::nothrow) ThreadSlot[new_slots_to_add];
    
    if (!new_slots_array) {
        return nullptr;
    }

    // 2. 初始化 Segment 结构体
    Segment new_segment;
    // 将原生指针的所有权转移给 unique_ptr，析构时会自动调用 delete[]
    new_segment.slots.reset(new_slots_array); 
    // 记录该段的大小
    new_segment.count = new_slots_to_add;

    // First commit ownership to the SegmentVector. This operation can
    // allocate and throw std::bad_alloc; until it succeeds, no pointer from
    // this array is visible to other threads, so the local unique_ptr can
    // safely destroy the array on failure.
    try {
        segments_.push_back(std::move(new_segment));
    } catch (const std::bad_alloc&) {
        return nullptr;
    }

    capacity_.fetch_add(new_slots_to_add, std::memory_order_relaxed);

    // Only now publish N-1 slots to the concurrent reuse stack. A pop()
    // may happen immediately, but segments_ already owns their memory.
    for (size_t i = 0; i + 1 < new_slots_to_add; ++i) {
        free_slots_.push(&new_slots_array[i]);
    }

    return &new_slots_array[new_slots_to_add - 1];
}

ThreadSlotManager::LocalSlotProxy::~LocalSlotProxy() {
    if(manager_ && slot_) {
        manager_->releaseSlot_(slot_);
    }
}