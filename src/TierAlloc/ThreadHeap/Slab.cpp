#include "ThreadHeap/Slab.hpp"
#include "common/GlobalConfig.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#ifdef TIERALLOC_CANARY
#include <mutex>
#include <set>
void canaryChunkRegister_(Slab* meta);
void canaryChunkUnregister_(Slab* meta);
#endif


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

#ifdef TIERALLOC_CANARY
    meta->canary_states_ = new uint8_t[meta->max_block_count_];
    std::memset(meta->canary_states_, 0, meta->max_block_count_);
    // 注册表登记放在 SizeClassPool::allocFromNew_（池生命周期入口），
    // 测试代码直接 CreateAt 复用内存属合法用法，不在此登记。
#endif

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
#ifdef TIERALLOC_CANARY
    canaryMarkFreeLocal_(ptr);
#endif
    *reinterpret_cast<void**>(ptr) = local_free_list_;
    local_free_list_ = ptr;
    allocated_count_--;

    return allocated_count_ == 0;
}


void Slab::freeRemote(void* ptr) {
#ifdef TIERALLOC_CANARY
    canaryMarkFreeRemote_(ptr);
#endif
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
#ifdef TIERALLOC_CANARY
        canaryMarkReclaim_(curr);
#endif
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
#ifdef TIERALLOC_CANARY
    canaryCheckPoison_(ptr, "allocFromList_");
    canaryMarkAlloc_(ptr, "allocFromList_");
#endif
    return ptr;
}

void* Slab::allocFromBump_() {
    void* ptr = static_cast<void*>(bump_ptr_);
    bump_ptr_ += block_size_;
    allocated_count_++;
#ifdef TIERALLOC_CANARY
    canaryMarkAlloc_(ptr, "allocFromBump_");
#endif
    return ptr;
}

void Slab::Destroy() {
#ifdef TIERALLOC_CANARY
    canaryPrepareReturn_();
#endif
#ifdef NDEBUG
    // 在 Release 模式下，只清理最危险的指针，性能开销最小
    this->owner_ = nullptr; 
#else
    // 在 Debug 模式下，用“毒药”值填充整个元数据区，
    constexpr size_t head_size = (sizeof(Slab) + kCacheLineSize - 1) & ~(kCacheLineSize - 1);
    memset(this, 0xDE, head_size); // 0xDEADBEEF...
#endif
}

#ifdef TIERALLOC_CANARY
namespace {
std::mutex& canaryRegistryMutex() {
    static std::mutex m;
    return m;
}
std::set<void*>& canaryActiveChunks() {
    static std::set<void*> s;
    return s;
}
} // namespace

void canaryChunkRegister_(Slab* meta) {
    std::lock_guard<std::mutex> lk(canaryRegistryMutex());
    if (!canaryActiveChunks().insert(static_cast<void*>(meta)).second) {
        std::fprintf(stderr,
            "[CANARY][FATAL] CHUNK-DOUBLE-HANDOUT | chunk=%p 被 CreateAt 二次激活"
            "（CentralHeap/ChunkCache 把同一块内存发给了两个 slab）\n",
            (void*)meta);
        std::abort();
    }
}

void canaryChunkUnregister_(Slab* meta) {
    std::lock_guard<std::mutex> lk(canaryRegistryMutex());
    canaryActiveChunks().erase(static_cast<void*>(meta));
}

void Slab::canaryPrepareReturn_() {
    if (canary_states_) {
        for (uint32_t i = 0; i < max_block_count_; ++i) {
            if (canary_states_[i] != 0) {
                std::fprintf(stderr,
                    "[CANARY][FATAL] RETURN-LIVE-CHUNK | slab=%p bs=%u block#%u "
                    "state=%u(0=free,1=alloc,2=remote) — 归还了含活块的 chunk！\n",
                    (void*)this, block_size_, i, canary_states_[i]);
                std::abort();
            }
        }
        delete[] canary_states_;
        canary_states_ = nullptr;
    }
    canaryChunkUnregister_(this);
}
#endif

#ifdef TIERALLOC_CANARY
size_t Slab::canaryIndexOf_(void* ptr) const {
    constexpr size_t head_size = (sizeof(Slab) + kCacheLineSize - 1) & ~(kCacheLineSize - 1);
    uintptr_t base = reinterpret_cast<uintptr_t>(this);
    uintptr_t first_block = base + head_size;
    uintptr_t p = reinterpret_cast<uintptr_t>(ptr);
    if (p < first_block || p >= reinterpret_cast<uintptr_t>(end_ptr_) ||
        (p - first_block) % block_size_ != 0) {
        std::fprintf(stderr,
            "[CANARY][FATAL] Slab:%p bs=%u | foreign/misaligned ptr=%p "
            "(first=%p end=%p)\n",
            (void*)this, block_size_, ptr, (void*)first_block, (void*)end_ptr_);
        std::abort();
    }
    return (p - first_block) / block_size_;
}

void Slab::canaryMarkAlloc_(void* ptr, const char* site) {
    size_t idx = canaryIndexOf_(ptr);
    uint8_t s = canary_states_[idx];
    if (s != 0) {
        std::fprintf(stderr,
            "[CANARY][FATAL] DOUBLE-ALLOC at %s | slab=%p block#%zu ptr=%p "
            "state=%u(0=free,1=alloc,2=remote)\n",
            site, (void*)this, idx, ptr, s);
        std::abort();
    }
    canary_states_[idx] = 1;
}

void Slab::canaryMarkFreeLocal_(void* ptr) {
    size_t idx = canaryIndexOf_(ptr);
    uint8_t s = canary_states_[idx];
    if (s != 1) {
        std::fprintf(stderr,
            "[CANARY][FATAL] BAD-LOCAL-FREE | slab=%p block#%zu ptr=%p "
            "state=%u(0=free,1=alloc,2=remote)\n",
            (void*)this, idx, ptr, s);
        std::abort();
    }
    canary_states_[idx] = 0;
    canaryPoison_(ptr);
}

void Slab::canaryMarkFreeRemote_(void* ptr) {
    size_t idx = canaryIndexOf_(ptr);
    uint8_t s = canary_states_[idx];
    if (s != 1) {
        std::fprintf(stderr,
            "[CANARY][FATAL] BAD-REMOTE-FREE (double free?) | slab=%p block#%zu "
            "ptr=%p state=%u(0=free,1=alloc,2=remote)\n",
            (void*)this, idx, ptr, s);
        std::abort();
    }
    canary_states_[idx] = 2;
    canaryPoison_(ptr);
}

void Slab::canaryMarkReclaim_(void* ptr) {
    size_t idx = canaryIndexOf_(ptr);
    uint8_t s = canary_states_[idx];
    if (s != 2) {
        std::fprintf(stderr,
            "[CANARY][FATAL] BAD-RECLAIM | slab=%p block#%zu ptr=%p "
            "state=%u(0=free,1=alloc,2=remote)\n",
            (void*)this, idx, ptr, s);
        std::abort();
    }
    canary_states_[idx] = 0;
}

void Slab::canaryPoison_(void* ptr) {
    // 首 8 字节被 freelist 链指针占用，投毒其余部分
    if (block_size_ > sizeof(void*)) {
        std::memset(reinterpret_cast<char*>(ptr) + sizeof(void*), 0xAA,
                    block_size_ - sizeof(void*));
    }
}

void Slab::canaryCheckPoison_(void* ptr, const char* site) {
    const char* p = reinterpret_cast<const char*>(ptr) + sizeof(void*);
    const char* end = reinterpret_cast<const char*>(ptr) + block_size_;
    for (; p < end; ++p) {
        if (static_cast<uint8_t>(*p) != 0xAA) {
            std::fprintf(stderr,
                "[CANARY][FATAL] UAF-WRITE at %s | slab=%p ptr=%p off=%td "
                "byte=%02x (expected AA) — 已释放块被游离指针写入\n",
                site, (void*)this, ptr, p - reinterpret_cast<const char*>(ptr),
                static_cast<uint8_t>(*p));
            std::abort();
        }
    }
}
#endif

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

