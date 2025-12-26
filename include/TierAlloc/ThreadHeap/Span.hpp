#pragma once 
#include "common/GlobalConfig.hpp"
#include "ThreadHeap/ChunkHeader.hpp"
#include <cstddef>
#include <new>

struct alignas(kCacheLineSize) Span : public ChunkHeader {
public:
    size_t total_bytes = 0;
    void* payload_start = nullptr;

    Span() : ChunkHeader(Type::LARGE) {};

    static Span* CreateAt(void* chunk_start, size_t requested_size) {
        Span* span = new (chunk_start) Span();

        span->total_bytes = requested_size;
        span->payload_start = reinterpret_cast<void*>(
            reinterpret_cast<uintptr_t>(chunk_start) + sizeof(Span));
        return span;
    }

    [[nodiscard]] size_t size() const { return total_bytes; }
    [[nodiscard]] void* payload() { return payload_start; }
    
};