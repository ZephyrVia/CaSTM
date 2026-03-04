#include "ThreadHeap/SizeClassPool.hpp"
#include "ThreadHeap/ThreadChunkCache.hpp"
#include "CentralHeap/CentralHeap.hpp"
#include "ThreadHeap/Slab.hpp"
#include "common/GlobalConfig.hpp"
#include <cassert>

SizeClassPool::~SizeClassPool() {
    // 线程退出时不能强制归还 Slab：
    // 其它线程可能仍持有这些对象（例如 EBR 延迟回收中的指针）。
    // 若在这里回收 chunk，会导致跨线程 UAF / double free。
    //
    // 当前策略：仅断开本地链表，保留 chunk 到进程结束。
    // 这是用空间换安全的保守策略，优先保证并发正确性。
    current_slab_ = nullptr;
    while (!partial_list_.empty()) {
        partial_list_.pop_front();
    }
    while (!full_list_.empty()) {
        full_list_.pop_front();
    }
}


void* SizeClassPool::allocate() {
    if(current_slab_) {
        void* ptr = current_slab_->allocate();
        if(ptr) return ptr;

        full_list_.push_back(current_slab_);
        current_slab_ = nullptr;
    }

    if(!partial_list_.empty()) {
        return allocFromPartial_();
    }

    if(!full_list_.empty()) {
        void* ptr = allocFromRescue_();
        if(ptr) return ptr;
    }

    return allocFromNew_();
}


void SizeClassPool::deallocate(Slab* slab, void* ptr) {
    assert(slab->owner() == this);

    bool was_full = slab->isFull();

    bool is_local_empty = slab->freeLocal(ptr);

    if(is_local_empty) {
        if(slab->reclaimRemoteMemory() > 0) {
            if(was_full) {
                full_list_.remove(slab);
                partial_list_.push_front(slab);
            }
        }
        else {
            if (current_slab_ == slab) {
                current_slab_ = nullptr;
            } 
            else if (was_full) {
                full_list_.remove(slab);
            } 
            else {
                partial_list_.remove(slab);
            }
            slab->Destroy();
            thread_chunk_cache_->returnChunk(reinterpret_cast<void*>(slab)); 
        }
    }
    else if (was_full && slab != current_slab_) {
        full_list_.remove(slab);
        partial_list_.push_front(slab);
    }
}

void* SizeClassPool::allocFromPartial_() {
    Slab* slab = partial_list_.pop_front();
    current_slab_ = slab;
    return current_slab_->allocate();
}

[[nodiscard]] void* SizeClassPool::allocFromRescue_() {
    int checks = 0;
    const size_t kMaxRescueChecks = kMaxPoolRescueChecks;

    while (!full_list_.empty() && checks < kMaxRescueChecks) {
        Slab* victim = full_list_.front();

        if(victim->reclaimRemoteMemory() > 0) {
            full_list_.remove(victim); 
            current_slab_ = victim;
            return current_slab_->allocate();
        } 
        else {
            full_list_.move_head_to_tail();
        }
        checks++;
    }
    return nullptr;
}

void* SizeClassPool::allocFromNew_() {
    void* chunk = thread_chunk_cache_->fetchChunk(); 
    if(chunk == nullptr)
        return nullptr;

    Slab* new_slab = Slab::CreateAt(chunk, this, block_size_);

    current_slab_ = new_slab;
    return current_slab_->allocate();
}
