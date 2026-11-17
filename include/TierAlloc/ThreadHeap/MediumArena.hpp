#pragma once

#include "common/GlobalConfig.hpp"

#include <cstddef>
#include <mutex>

// Process-lifetime allocator for medium requests.
//
// Each 2 MiB-aligned Chunk contains one 4 KiB metadata page, followed by
// 511 independently allocatable 4 KiB pages. Contiguous page runs allow
// several medium objects to share a Chunk without per-object headers.
//
// The allocator is intentionally process-wide. Unlike ThreadHeap's small
// Slabs, its lifetime is not tied to any thread's TLS destructor.
class MediumArena {
public:
    static constexpr std::size_t kPageSize = 4 * 1024;
    static constexpr std::size_t kPageCount = kChunkSize / kPageSize;
    static constexpr std::size_t kMaxAlloc = kChunkSize - kPageSize;

    static MediumArena& GetInstance() noexcept;

    [[nodiscard]] void* allocate(std::size_t bytes) noexcept;
    void deallocate(void* ptr) noexcept;

    MediumArena(const MediumArena&) = delete;
    MediumArena& operator=(const MediumArena&) = delete;

private:
    struct MediumChunk;

    MediumArena() = default;
    ~MediumArena() = default;

    std::mutex mutex_;
    MediumChunk* chunks_ = nullptr;
};
