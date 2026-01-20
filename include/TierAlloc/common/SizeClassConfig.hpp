#pragma once

#include "GlobalConfig.hpp"
#include <cstddef>
#include <cstdint>


class SizeClassConfig {
public:
    static constexpr size_t kMinAlloc = 8;
    static constexpr size_t kMaxAlloc = 256 * 1024;
    static constexpr size_t kAlignment = 8;
    static constexpr size_t kPageSize = 4 * 1024;
    static constexpr size_t kSlabSize = kChunkSize;

    static constexpr size_t kClassCount = 104;
    [[nodiscard]] static constexpr std::size_t ClassCount() noexcept { return kClassCount; }

public:
    static void Init();

    [[nodiscard]] static uint32_t SizeToClass(size_t nbytes) noexcept;
    [[nodiscard]] static size_t ClassToSize(size_t class_idx) noexcept;
    [[nodiscard]] static size_t Normalize(size_t nbytes) noexcept;

private:
    [[nodiscard]] static inline size_t RoundUp_(size_t nbytes, size_t align) {
        return ((nbytes + align -1) & ~(align -1));
    }
    static size_t class_to_size_[kClassCount];
};