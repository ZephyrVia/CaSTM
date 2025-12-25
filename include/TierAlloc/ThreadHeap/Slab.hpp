#pragma once 
#include "common/GlobalConfig.hpp"
#include "common/AtomicFreeList.hpp"

#include <cstdint>

class SizeClassPool;

struct alignas(kCacheLineSize) Slab {
public:
    static Slab* CreateAt(void* chunk_start, SizeClassPool* pool, uint32_t  block_size);

    [[nodiscard]] void* allocate();
    bool freeLocal(void* ptr);
    void freeRemote(void* ptr);
    uint32_t reclaim_remote_memory();
    
    [[nodiscard]] uint32_t block_size() const;
    [[nodiscard]] uint32_t max_block_count() const;
    [[nodiscard]] uint32_t allocated_count() const;
    [[nodiscard]] SizeClassPool* owner() const;

    [[nodiscard]] bool isFull() const;
    [[nodiscard]] bool isEmpty() const;

    Slab* prev = nullptr;
    Slab* next = nullptr;

private:
    Slab() = default;

    void* local_free_list_ = nullptr;

    SizeClassPool* owner_ = nullptr;
    
    char* bump_ptr_ = nullptr;
    char* end_ptr_ = nullptr;

    uint32_t block_size_ = 0;
    uint32_t max_block_count_ = 0;
    uint32_t allocated_count_ = 0;

    alignas(kCacheLineSize) AtomicFreeList remote_free_list_{}; 
};