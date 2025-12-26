#include "ThreadHeap/SizeClassPool.hpp"
#include "ThreadHeap/ThreadChunkCache.hpp"
#include "CentralHeap/CentralHeap.hpp"
#include "ThreadHeap/Slab.hpp"
#include "common/GlobalConfig.hpp"
#include <cassert>

SizeClassPool::~SizeClassPool() {
    auto& central = CentralHeap::GetInstance();

    // [KNOWN LIMITATION / 已知缺陷]
    // 警告：当前析构逻辑采取“暴力回收”策略。
    // 当线程退出时，无论 Slab 中是否有对象仍被其他线程持有，都会强制将内存归还给 CentralHeap。
    //
    // 风险点：如果线程 T2 仍持有由本线程(T1)分配的内存，T1 退出后，
    //        T2 尝试访问或释放(free)该内存时会触发 Use-After-Free (SegFault)。
    //
    // 适用场景：仅适用于线程池架构（线程不频繁销毁）或无跨线程内存传递的简单场景。
    
    if(current_slab_ != nullptr) {
        central.returnChunk(reinterpret_cast<void*>(current_slab_));
    }

    while(!partial_list_.empty()) {
        Slab* slab = partial_list_.pop_front();
        central.returnChunk(reinterpret_cast<void*>(slab));
    }

    while(!full_list_.empty()) {
        Slab* slab = full_list_.pop_front();
        central.returnChunk(reinterpret_cast<void*>(slab));
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