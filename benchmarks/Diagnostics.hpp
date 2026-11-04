#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <limits>
#include <new>
#include <string>
#include <utility>

namespace stm_bench {

enum class AbortCause : unsigned {
    None,
    SnapshotUnavailable,
    ReadLocked,
    ReadChanged,
    WwInactive,
    WwCommit,
    Exception,
    Count
};

inline constexpr std::array<const char*, static_cast<unsigned>(AbortCause::Count)>
    kAbortNames{{"none", "snapshot_unavailable", "read_locked", "read_changed",
                 "ww_inactive", "ww_commit", "exception"}};

enum class Failure {
    None,
    RetryLimit,
    AllocationFailure,
    Exception,
    UnknownException
};

inline const char* failureName(Failure failure) noexcept {
    switch (failure) {
    case Failure::None: return "none";
    case Failure::RetryLimit: return "retry_limit";
    case Failure::AllocationFailure: return "allocation_failure";
    case Failure::Exception: return "exception";
    case Failure::UnknownException: return "unknown_exception";
    }
    return "unknown";
}

struct LogicalOutcome {
    bool valid = true;
    bool committed = false;
    uint64_t attempts = 0;
    uint64_t aborts = 0;
    uint64_t writes = 0;
    std::array<uint64_t, kAbortNames.size()> causes{};
    Failure failure = Failure::None;
    std::string error;

    void aborted(AbortCause cause) noexcept {
        ++aborts;
        ++causes[static_cast<unsigned>(cause)];
    }
};

// Also used with scripted attempts in tests: terminal failures retain all
// attempts, including the throwing attempt and the retry-guard boundary.
template<class Attempt, class OnRetry>
LogicalOutcome runAttempts(uint64_t max_attempts, Attempt&& attempt, OnRetry&& on_retry) {
    LogicalOutcome outcome;
    while (outcome.attempts < max_attempts) {
        ++outcome.attempts;
        try {
            const auto cause = attempt();
            if (cause == AbortCause::None) {
                outcome.committed = true;
                return outcome;
            }
            outcome.aborted(cause);
        } catch (const std::bad_alloc& error) {
            outcome.aborted(AbortCause::Exception);
            outcome.failure = Failure::AllocationFailure;
            outcome.error = error.what();
            break;
        } catch (const std::exception& error) {
            outcome.aborted(AbortCause::Exception);
            outcome.failure = Failure::Exception;
            outcome.error = error.what();
            break;
        } catch (...) {
            outcome.aborted(AbortCause::Exception);
            outcome.failure = Failure::UnknownException;
            outcome.error = "non-standard exception";
            break;
        }
        // Never wait after success or after exhausting the retry budget.
        if (outcome.attempts < max_attempts) on_retry(outcome.attempts);
    }
    outcome.valid = false;
    if (outcome.failure == Failure::None) outcome.failure = Failure::RetryLimit;
    return outcome;
}

template<class Attempt>
LogicalOutcome runAttempts(uint64_t max_attempts, Attempt&& attempt) {
    return runAttempts(max_attempts, std::forward<Attempt>(attempt), [](uint64_t) noexcept {});
}

struct PhaseStats {
    uint64_t committed = 0;
    uint64_t failed = 0;
    uint64_t attempts = 0;
    uint64_t aborts = 0;
    uint64_t writes = 0;
    uint64_t max_retries = 0;
    std::array<uint64_t, kAbortNames.size()> causes{};
    // Retries per completed logical transaction (success OR terminal failure):
    // 0, 1, 2..3, 4..7, ..., 2^63..UINT64_MAX.
    std::array<uint64_t, 65> retry_histogram{};

    void record(const LogicalOutcome& outcome) noexcept {
        committed += outcome.committed ? 1 : 0;
        failed += outcome.committed ? 0 : 1;
        attempts += outcome.attempts;
        aborts += outcome.aborts;
        if (outcome.committed) writes += outcome.writes;
        for (unsigned i = 0; i < causes.size(); ++i) causes[i] += outcome.causes[i];
        const uint64_t retries = outcome.attempts ? outcome.attempts - 1 : 0;
        max_retries = std::max(max_retries, retries);
        unsigned bucket = 0;
        for (uint64_t n = retries; n; n >>= 1) ++bucket;
        ++retry_histogram[bucket];
    }

    void merge(const PhaseStats& other) noexcept {
        committed += other.committed;
        failed += other.failed;
        attempts += other.attempts;
        aborts += other.aborts;
        writes += other.writes;
        max_retries = std::max(max_retries, other.max_retries);
        for (unsigned i = 0; i < causes.size(); ++i) causes[i] += other.causes[i];
        for (unsigned i = 0; i < retry_histogram.size(); ++i)
            retry_histogram[i] += other.retry_histogram[i];
    }

    uint64_t retryPercentileUpper(unsigned percent) const noexcept {
        const uint64_t total = committed + failed;
        if (!total) return 0;
        const uint64_t rank = (total / 100) * percent +
                             ((total % 100) * percent + 99) / 100;
        uint64_t cumulative = 0;
        for (unsigned i = 0; i < retry_histogram.size(); ++i) {
            cumulative += retry_histogram[i];
            if (cumulative >= rank) {
                const uint64_t upper = i == 64 ? std::numeric_limits<uint64_t>::max()
                                               : (uint64_t{1} << i) - 1;
                return std::min(upper, max_retries);
            }
        }
        return max_retries;
    }
};

} // namespace stm_bench
