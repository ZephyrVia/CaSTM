//EBRManager/EBRManager.hpp
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include "EBRManager/ThreadSlotManager.hpp"
#include "EBRManager/GarbageCollector.hpp"
#include "EBRManager/LockFreeSingleLinkedList.hpp"
#include "TierAlloc/ThreadHeap/ThreadHeap.hpp"

class EBRManager {
public:
    EBRManager();
    ~EBRManager();

    EBRManager(const EBRManager&) = delete;
    EBRManager& operator=(const EBRManager&) = delete;
    EBRManager(const EBRManager&&) = delete;
    EBRManager& operator=(const EBRManager&&) = delete;

    static EBRManager* instance() {
        static EBRManager instance;
        return &instance;
    }

    void enter();
    void leave();

    template<typename T>
    void retire(T* ptr);
    void retire(void* ptr, void (*deleter)(void*));

public:
    static constexpr size_t kNumEpochLists = 3;

private:
    bool tryAdvanceEpoch_();
    void collectGarbage_(uint64_t epoch_to_collect);
    ThreadSlot* getLocalSlot_();

private:
    alignas(64) std::atomic<uint64_t> global_epoch_;
    LockFreeSingleLinkedList garbage_lists_[kNumEpochLists];

    ThreadSlotManager slot_manager_;
    GarbageCollector garbage_collector_;
};


template<typename T>
void EBRManager::retire(T* ptr) {
    if(ptr == nullptr){
        return;
    }
    
    auto default_deleter = [](void* p) {
        T* typed_p   = static_cast<T*>(p);
        typed_p->~T();
        ThreadHeap::deallocate(typed_p);
    };

    this->retire(static_cast<void*>(ptr), default_deleter);
}
