#include "ThreadHeap/ThreadHeap.hpp"
#include "ThreadHeap/Slab.hpp"
#include "ThreadHeap/Span.hpp"
#include "ThreadHeap/MediumArena.hpp"
#include "ThreadHeap/ThreadHeapDepot.hpp"
#include "CentralHeap/CentralHeap.hpp"
#include "ThreadHeap/ChunkHeader.hpp"
#include "common/SizeClassConfig.hpp"
#include <cstdint>
#include <limits>
#include <cassert>


[[nodiscard]] void* ThreadHeap::allocate(size_t nbytes) noexcept {
    // Small: existing per-thread size-class Slabs.
    if (nbytes <= SizeClassConfig::kMaxAlloc) [[likely]] {
        ThreadHeap* heap = local_();
        if (!heap) return nullptr;
        uint32_t class_idx = SizeClassConfig::SizeToClass(nbytes);
        return heap->pools_[class_idx].allocate();
    }

    // Medium: multiple allocations share one 2 MiB-aligned Chunk.
    if (nbytes <= MediumArena::kMaxAlloc) {
        return MediumArena::GetInstance().allocate(nbytes);
    }

    // Large: reserve a whole number of aligned 2 MiB Chunks so that
    // SystemChunkAllocator's mmap and munmap extents always match.
    constexpr size_t kRoundUp = kChunkSize - 1;
    constexpr size_t kMax = std::numeric_limits<size_t>::max();
    if (nbytes > kMax - sizeof(Span) - kRoundUp) {
        return nullptr;
    }
    const size_t total = nbytes + sizeof(Span);
    const size_t mapped_bytes = (total + kRoundUp) & kChunkMask;
    if (mapped_bytes > kMax - kChunkAlignment) {
        // SystemChunkAllocator over-allocates by one alignment unit.
        return nullptr;
    }

    void* chunk_start = CentralHeap::GetInstance().allocateLarge(mapped_bytes);
    if (!chunk_start) {
        return nullptr;
    }
    Span* span = Span::CreateAt(chunk_start, mapped_bytes);
    return span->payload();
}

void ThreadHeap::deallocate(void* ptr) noexcept {
    if (!ptr) [[unlikely]] {
        return;
    }

    ChunkHeader* header = ChunkHeader::Get(ptr);
    if (header->type == ChunkHeader::Type::SMALL) [[likely]] {
        Slab* slab = static_cast<Slab*>(header);
        ThreadHeap* heap = local_();

        if (heap && heap->isOwnSlab_(slab)) {
            slab->owner()->deallocate(slab, ptr);
        } else {
            slab->freeRemote(ptr);
        }
        return;
    }

    if (header->type == ChunkHeader::Type::MEDIUM) {
        MediumArena::GetInstance().deallocate(ptr);
        return;
    }

    assert(header->type == ChunkHeader::Type::LARGE);
    Span* span = static_cast<Span*>(header);
    CentralHeap::GetInstance().freeLarge(span, span->size());
}

ThreadHeap* ThreadHeap::local_() noexcept {
    // TLS owns only an exclusive-use lease, not the Slabs' backing memory.
    static thread_local ThreadHeapLease lease;
    return lease.get();
}


ThreadHeap::ThreadHeap() noexcept {
    SizeClassConfig::Init();
   
    for (uint32_t i = 0; i < SizeClassConfig::kClassCount; ++i) {
        uint32_t block_size = SizeClassConfig::ClassToSize(i);
        pools_[i].Init(block_size, &chunk_cache_);
    }
}

[[nodiscard]] bool ThreadHeap::isOwnSlab_(const Slab* slab) const noexcept{
    uint32_t class_idx = SizeClassConfig::SizeToClass(slab->block_size());
    
    const SizeClassPool* expected_pool = &pools_[class_idx];
    return expected_pool == slab->owner();
}

