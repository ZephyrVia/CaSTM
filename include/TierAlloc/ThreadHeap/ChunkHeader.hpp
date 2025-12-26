#pragma once 
#include "common/GlobalConfig.hpp"

#include <cassert>
#include <cstdint>

struct alignas(kCacheLineSize) ChunkHeader {
public:
    enum class Type : uint8_t {
        SMALL = 0,
        LARGE = 1
    };

    Type    type = Type::SMALL;

    ChunkHeader(Type t = Type::SMALL) : type(t) {};

    [[nodiscard]] static inline ChunkHeader* Get(void* ptr) {
        return reinterpret_cast<ChunkHeader*>(
            reinterpret_cast<uintptr_t>(ptr) & (kChunkMask));
    }
};