#pragma once

#include <atomic>
#include <cstdint>

namespace STM {
namespace Ww {

class alignas(64) GlobalClock {
public:
    GlobalClock() = delete;
    GlobalClock(const GlobalClock&) = delete;
    GlobalClock& operator=(const GlobalClock&) = delete;

    static uint64_t now() noexcept {
        return clock_.load(std::memory_order_acquire);
    }

    static uint64_t tick() noexcept {
        return clock_.fetch_add(1, std::memory_order_acq_rel) + 1;
    }

private:
    inline static std::atomic<uint64_t> clock_{0};
};

}
}