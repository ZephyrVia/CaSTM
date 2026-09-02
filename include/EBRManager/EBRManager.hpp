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
    EBRManager(const EBRManager&) = delete;
    EBRManager& operator=(const EBRManager&) = delete;
    EBRManager(const EBRManager&&) = delete;
    EBRManager& operator=(const EBRManager&&) = delete;

    static EBRManager* instance() {
        static EBRManager* instance = new EBRManager();
        return instance;
    }

    void enter();
    void leave();

    template<typename T>
    void retire(T* ptr);
    void retire(void* ptr, void (*deleter)(void*));

public:
    static constexpr size_t kNumEpochLists = 3;

private:
    EBRManager();
    ~EBRManager();
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

    // 约定：默认 deleter 使用 delete，依赖类型自身的 operator delete 分派
    // （Occ 的 VersionNode → ThreadHeap；Ww 的 VersionNode/WriteRecord → 系统堆）。
    // 从 ThreadHeap 裸分配（ThreadHeap::allocate + placement new）的对象
    // 必须提供类内 operator new/delete，否则需显式传 deleter。
    auto default_deleter = [](void* p) {
        T* typed_p   = static_cast<T*>(p);
        delete typed_p;
    };

    this->retire(static_cast<void*>(ptr), default_deleter);
}
