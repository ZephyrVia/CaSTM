#include "CentralHeap/CentralHeap.hpp"
#include "common/GlobalConfig.hpp"
#include <cassert>
#include <cstddef>

CentralHeap& CentralHeap::GetInstance() {
    static CentralHeap instance;
    return instance;
}

[[nodiscard]] void* CentralHeap::fetchChunk() {
    void* ptr = free_list_.try_pop();
    if (ptr != nullptr) {
        return ptr;
    }

    return system_allocator_.allocate(kChunkSize);
}


void CentralHeap::returnChunk(void* ptr) {
    if(ptr == nullptr) 
        return;

    assert((reinterpret_cast<uintptr_t>(ptr) & (kChunkAlignment - 1 )) == 0);

    if(free_list_.size() >= kMaxCentralCacheSize) {
        system_allocator_.deallocate(ptr, kChunkSize);
        return;
    }

    free_list_.push(ptr);
    return;
}


size_t CentralHeap::getFreeChunkCount() {
    return free_list_.size();
}


void* CentralHeap::allocateLarge(size_t nbytes) {

    if (nbytes <= kChunkSize) {
        return fetchChunk();
    }

    return system_allocator_.allocate(nbytes);
}


void CentralHeap::freeLarge(void* ptr, size_t nbytes) {
    if (!ptr) return;

    // Case 1: 归还的是单个 Chunk
    if (nbytes <= kChunkSize) {
        returnChunk(ptr);
        return;
    }

    // Case 2: 归还的是超大内存
    system_allocator_.deallocate(ptr, nbytes);
}