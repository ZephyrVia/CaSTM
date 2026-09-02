#pragma once 
#include "common/GlobalConfig.hpp"
#include "common/AtomicFreeList.hpp"
#include "ThreadHeap/ChunkHeader.hpp"

#include <cassert>
#include <cstdint>

class SizeClassPool;


struct alignas(kCacheLineSize) Slab : public ChunkHeader {
public:
    static Slab* CreateAt(void* chunk_start, SizeClassPool* pool, uint32_t  block_size);

    [[nodiscard]] static inline Slab* GetSlab(void* ptr) {
        ChunkHeader* header = ChunkHeader::Get(ptr);
        assert(header->type == Type::SMALL); 
        return static_cast<Slab*>(header);
    }

    [[nodiscard]] void* allocate();
    bool freeLocal(void* ptr);
    void freeRemote(void* ptr);
    uint32_t reclaimRemoteMemory();
    void Destroy();
    
    [[nodiscard]] uint32_t block_size() const;
    [[nodiscard]] uint32_t max_block_count() const;
    [[nodiscard]] uint32_t allocated_count() const;
    [[nodiscard]] SizeClassPool* owner() const;

    [[nodiscard]] bool isFull() const;
    [[nodiscard]] bool isEmpty() const;

#ifdef TIERALLOC_CANARY
    // chunk 归还前的终检：所有块必须处于 free 状态，并从全局活跃注册表摘除。
    void canaryPrepareReturn_();
#endif

    Slab* prev = nullptr;
    Slab* next = nullptr;

private:
    Slab() = default;
    void* allocFromList_();
    void* allocFromBump_();

    void* local_free_list_ = nullptr;

    SizeClassPool* owner_ = nullptr;
    
    char* bump_ptr_ = nullptr;
    char* end_ptr_ = nullptr;

    uint32_t block_size_ = 0;
    uint32_t max_block_count_ = 0;
    uint32_t allocated_count_ = 0;

#ifdef TIERALLOC_CANARY
    // 调试金丝雀：块状态 side-bitmap（0=free, 1=allocated, 2=remote-freed）
    // 不放在块内，因为空闲块首字被 freelist 链指针占用。
    uint8_t* canary_states_ = nullptr;
    size_t canaryIndexOf_(void* ptr) const;
    void canaryMarkAlloc_(void* ptr, const char* site);
    void canaryMarkFreeLocal_(void* ptr);
    void canaryMarkFreeRemote_(void* ptr);
    void canaryMarkReclaim_(void* ptr);
    void canaryPoison_(void* ptr);
    void canaryCheckPoison_(void* ptr, const char* site);
#endif

    alignas(kCacheLineSize) AtomicFreeList remote_free_list_{};
};