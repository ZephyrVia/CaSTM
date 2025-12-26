#include "ThreadHeap/Slab.hpp"
#include "common/GlobalConfig.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>


Slab* Slab::CreateAt(void *chunk_start, SizeClassPool *pool, uint32_t block_size) {
    assert(chunk_start != nullptr);
    assert(block_size >= sizeof(void*));

    Slab* meta = new (chunk_start) Slab();

    meta->owner_ = pool;
    meta->block_size_ = block_size;

    uintptr_t base = reinterpret_cast<uintptr_t>(chunk_start); 
    size_t meta_size = sizeof(Slab);
    size_t head_size = (meta_size + kCacheLineSize - 1) & ~(kCacheLineSize - 1);
    
    meta->bump_ptr_ = reinterpret_cast<char*>(base + head_size);
    meta->end_ptr_ = reinterpret_cast<char*>(base + kChunkSize);
    
    size_t avail_bytes = kChunkSize - head_size;
    meta->max_block_count_ = static_cast<uint32_t>(avail_bytes / block_size);

    meta->allocated_count_ = 0;
    meta->local_free_list_ = nullptr;

    return meta;
}

[[nodiscard]] void* Slab::allocate() {
    if(local_free_list_ != nullptr) {
        return allocFromList_();
    }

    if (!remote_free_list_.empty()) {
        if(reclaimRemoteMemory() > 0) {
            return allocFromList_();
        }
    }
    
    if(bump_ptr_ + block_size_ <= end_ptr_) {
        return allocFromBump_();
    }

    return nullptr;
}


bool Slab::freeLocal(void* ptr) {
    *reinterpret_cast<void**>(ptr) = local_free_list_;
    local_free_list_ = ptr;
    allocated_count_--;

    return allocated_count_ == 0;
}


void Slab::freeRemote(void* ptr) {
    remote_free_list_.push(ptr);
}

uint32_t Slab::reclaimRemoteMemory() {
    void* head = remote_free_list_.steal_all();
    if(head == nullptr)
        return 0;

    uint32_t count = 0;
    void* curr = head;
    void* tail = nullptr;

    while (curr) {
        tail = curr;
        count++;
        curr = *reinterpret_cast<void**>(curr);
    }

    *reinterpret_cast<void**>(tail) = local_free_list_;
    local_free_list_ = head;
    allocated_count_ -= count;

    return count;
}

void* Slab::allocFromList_() {
    void* ptr = local_free_list_;
    local_free_list_ = *reinterpret_cast<void**>(ptr);
    allocated_count_++;
    return ptr;
}

void* Slab::allocFromBump_() {
    void* ptr = static_cast<void*>(bump_ptr_);
    bump_ptr_ += block_size_;
    allocated_count_++;
    return ptr;
}

void Slab::Destroy() {
#ifdef NDEBUG
    // 在 Release 模式下，只清理最危险的指针，性能开销最小
    this->owner_ = nullptr; 
#else
    // 在 Debug 模式下，用“毒药”值填充整个元数据区，
    constexpr size_t head_size = (sizeof(Slab) + kCacheLineSize - 1) & ~(kCacheLineSize - 1);
    memset(this, 0xDE, head_size); // 0xDEADBEEF...
#endif
}

uint32_t Slab::block_size() const {
    return block_size_;
}

uint32_t Slab::max_block_count() const {
    return max_block_count_;
}

uint32_t Slab::allocated_count() const {
    return allocated_count_;
}

SizeClassPool* Slab::owner() const {
    return owner_;
}

bool Slab::isFull() const {
    return allocated_count_ == max_block_count_;
}

bool Slab::isEmpty() const {
    return allocated_count_ == 0;
}

