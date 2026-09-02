#include "ThreadHeap/SizeClassPool.hpp"
#include "ThreadHeap/ThreadChunkCache.hpp"
#include "CentralHeap/CentralHeap.hpp"
#include "ThreadHeap/Slab.hpp"
#include "common/GlobalConfig.hpp"
#include <cassert>

#ifdef TIERALLOC_CANARY
void canaryChunkRegister_(Slab* meta);
#endif

SizeClassPool::~SizeClassPool() {
    // 线程退出时绝不归还 chunk（含活块归还是并发踩踏的根因）：
    // 本线程 slab 中的对象可能仍被全局引用——共享 TMVar 版本链上的
    // VersionNode、EBR 全局垃圾链表里的 GarbageNode 等。CentralHeap
    // 复用这些 chunk 后，新分配会直接覆盖活对象（已由 TIERALLOC_CANARY
    // 在 ~SizeClassPool 路径捕获 RETURN-LIVE-CHUNK 实锤）。
    // 策略：只断开本地链表，chunk 保留到进程结束（空间换安全）。
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
            // 空板不销毁、不归还 CentralHeap：其他线程的 freeRemote 可能
            // 仍指向本 chunk 的块（destroy 与并发 remote push 之间存在窗口），
            // 归还复用会覆盖尚被引用的内存。空板留在 partial_list_ 中复用。
            if (current_slab_ == slab) {
                current_slab_ = nullptr;
                partial_list_.push_front(slab);   // current 板不在任何链表
            }
            else if (was_full) {
                full_list_.remove(slab);
                partial_list_.push_front(slab);
            }
            // else: 本就在 partial_list_ 中，留原地即可
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
#ifdef TIERALLOC_CANARY
    canaryChunkRegister_(new_slab);
#endif

    current_slab_ = new_slab;
    return current_slab_->allocate();
}