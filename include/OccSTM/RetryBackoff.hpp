#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <thread>

namespace STM::Occ {

enum class BackoffPolicy { None, Yield, Jitter };

// Call only after an aborted attempt has released its write locks. Attempts 1
// and 2 yield; subsequent failures sleep for a thread-local random duration in
// [0, min(2^(failures-3), 64)] microseconds. OS scheduling may overshoot the cap.
// The caller supplies the consecutive failure count, resetting it per logical
// transaction. No global RNG, counters or synchronization is introduced.
class RetryBackoff {
public:
    static constexpr uint64_t kMaxDelayUs = 64;

    explicit RetryBackoff(BackoffPolicy policy, uint64_t seed) noexcept
        : policy_(policy), state_(seed ? seed : 0xCA57BEEF) {}

    static uint64_t windowUs(uint64_t failures) noexcept {
        if (failures <= 2) return 0;
        return uint64_t{1} << std::min<uint64_t>(failures - 3, 6);
    }

    uint64_t nextDelayUs(uint64_t failures) noexcept {
        const uint64_t window = windowUs(failures);
        if (!window) return 0;
        state_ ^= state_ >> 12;
        state_ ^= state_ << 25;
        state_ ^= state_ >> 27;
        return (state_ * 0x2545F4914F6CDD1DULL) % (window + 1);
    }

    void wait(uint64_t failures) noexcept {
        if (policy_ == BackoffPolicy::None) return;
        if (policy_ == BackoffPolicy::Yield || failures <= 2) {
            std::this_thread::yield();
            return;
        }
        const auto delay = nextDelayUs(failures);
        if (delay) std::this_thread::sleep_for(std::chrono::microseconds(delay));
        else std::this_thread::yield();
    }

private:
    BackoffPolicy policy_;
    uint64_t state_;
};

} // namespace STM::Occ
