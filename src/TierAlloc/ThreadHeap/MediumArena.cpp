#include "ThreadHeap/MediumArena.hpp"

#include "CentralHeap/CentralHeap.hpp"
#include "ThreadHeap/ChunkHeader.hpp"

#include <cassert>
#include <cstdint>
#include <limits>
#include <new>

static_assert(kChunkSize % MediumArena::kPageSize == 0,
              "Chunk size must be divisible by medium page size");
static_assert(MediumArena::kPageCount <=
                  std::numeric_limits<std::uint16_t>::max(),
              "Run length must fit in uint16_t");

struct alignas(kCacheLineSize) MediumArena::MediumChunk final : ChunkHeader {
    std::uint64_t used_bitmap[kPageCount / 64] = {};
    std::uint16_t run_length[kPageCount] = {};
    std::uint32_t live_allocations = 0;
    MediumChunk* next = nullptr;

    MediumChunk() noexcept : ChunkHeader(Type::MEDIUM) {
        used_bitmap[0] = 1;  // Reserve physical page zero for metadata.
    }

    [[nodiscard]] bool isUsed(std::size_t page) const noexcept {
        return (used_bitmap[page / 64] &
                (std::uint64_t{1} << (page % 64))) != 0;
    }

    void markUsed(std::size_t start, std::size_t count, bool used) noexcept {
        for (std::size_t i = start; i < start + count; ++i) {
            const auto mask = std::uint64_t{1} << (i % 64);
            if (used) {
                assert(!isUsed(i));
                used_bitmap[i / 64] |= mask;
            } else {
                assert(isUsed(i));
                used_bitmap[i / 64] &= ~mask;
            }
        }
    }

    [[nodiscard]] bool findRun(std::size_t needed,
                               std::size_t& start) const noexcept {
        std::size_t run = 0;
        for (std::size_t page = 1; page < kPageCount; ++page) {
            run = isUsed(page) ? 0 : run + 1;
            if (run == needed) {
                start = page + 1 - needed;
                return true;
            }
        }
        return false;
    }
};

MediumArena& MediumArena::GetInstance() noexcept {
    // Deliberately process-lifetime: no global destructor may return chunks
    // while other threads are still using this allocator at shutdown.
    static auto* instance = new MediumArena();
    return *instance;
}

void* MediumArena::allocate(std::size_t bytes) noexcept {
    static_assert(sizeof(MediumChunk) <= kPageSize,
                  "MediumChunk metadata exceeds the first page");
    if (bytes == 0 || bytes > kMaxAlloc) {
        return nullptr;
    }

    const std::size_t needed = (bytes + kPageSize - 1) / kPageSize;
    std::lock_guard<std::mutex> guard(mutex_);

    MediumChunk* selected = nullptr;
    std::size_t start = 0;

    for (auto* chunk = chunks_; chunk; chunk = chunk->next) {
        if (chunk->findRun(needed, start)) {
            selected = chunk;
            break;
        }
    }

    if (!selected) {
        void* memory = CentralHeap::GetInstance().fetchChunk();
        if (!memory) {
            return nullptr;
        }
        assert((reinterpret_cast<std::uintptr_t>(memory) &
                (kChunkAlignment - 1)) == 0);
        selected = new (memory) MediumChunk();
        selected->next = chunks_;
        chunks_ = selected;
        start = 1;  // Every new chunk has 511 consecutive free data pages.
    }

    assert(start > 0 && start + needed <= kPageCount);
    assert(selected->run_length[start] == 0);
    selected->markUsed(start, needed, true);
    selected->run_length[start] = static_cast<std::uint16_t>(needed);
    ++selected->live_allocations;

    return reinterpret_cast<char*>(selected) + start * kPageSize;
}

void MediumArena::deallocate(void* ptr) noexcept {
    if (!ptr) {
        return;
    }

    const auto address = reinterpret_cast<std::uintptr_t>(ptr);
    const auto base = address & static_cast<std::uintptr_t>(kChunkMask);
    const std::size_t offset = address - base;

    assert(offset >= kPageSize && offset < kChunkSize);
    assert(offset % kPageSize == 0);

    auto* chunk = reinterpret_cast<MediumChunk*>(base);
    std::lock_guard<std::mutex> guard(mutex_);
    assert(chunk->type == ChunkHeader::Type::MEDIUM);

    const std::size_t start = offset / kPageSize;
    const std::size_t count = chunk->run_length[start];
    assert(count > 0 && start + count <= kPageCount);
    assert(chunk->live_allocations > 0);

    chunk->markUsed(start, count, false);
    chunk->run_length[start] = 0;
    --chunk->live_allocations;

    if (chunk->live_allocations != 0) {
        return;
    }

    // No active allocations remain. Any memory reclaimed through EBR must
    // have passed its own grace-period check before reaching this function.
    MediumChunk** link = &chunks_;
    while (*link && *link != chunk) {
        link = &(*link)->next;
    }
    assert(*link == chunk);
    *link = chunk->next;
    chunk->~MediumChunk();
    CentralHeap::GetInstance().returnChunk(chunk);
}
