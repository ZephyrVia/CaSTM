#pragma once

#include <atomic>
#include <cstdint>

namespace STM {
namespace Occ {

class alignas(64) GlobalClock {
public:
    GlobalClock() = delete;
    GlobalClock(const GlobalClock&) = delete;
    GlobalClock& operator=(const GlobalClock&) = delete;

    static uint64_t now() noexcept {
        return clock_.load(std::memory_order_seq_cst);
    }

    static uint64_t tick() noexcept {
        return clock_.fetch_add(1, std::memory_order_seq_cst) + 1;
    }

private:
    inline static std::atomic<uint64_t> clock_{0};
};

}
}