#include <gtest/gtest.h>
#include <stdexcept>
#include <vector>
#include "Diagnostics.hpp"
#include "OccSTM/RetryBackoff.hpp"

using namespace stm_bench;

TEST(BenchmarkDiagnosticsTest, TerminalRetryGuardRetainsAllAttemptsAndCauses) {
    auto result = runAttempts(5, [] { return AbortCause::ReadLocked; });
    EXPECT_FALSE(result.valid);
    EXPECT_FALSE(result.committed);
    EXPECT_EQ(result.failure, Failure::RetryLimit);
    EXPECT_EQ(result.attempts, 5u);
    EXPECT_EQ(result.aborts, 5u);
    EXPECT_EQ(result.causes[static_cast<unsigned>(AbortCause::ReadLocked)], 5u);
    PhaseStats stats;
    stats.record(result);
    EXPECT_EQ(stats.failed, 1u);
    EXPECT_EQ(stats.attempts, 5u);
    EXPECT_EQ(stats.max_retries, 4u);
    EXPECT_EQ(stats.retry_histogram[3], 1u);
}

TEST(BenchmarkDiagnosticsTest, MixedAbortsAndSuccessAreCountedOnce) {
    int step = 0;
    const AbortCause causes[]{AbortCause::SnapshotUnavailable, AbortCause::ReadChanged, AbortCause::None};
    auto result = runAttempts(5, [&] { return causes[step++]; });
    EXPECT_TRUE(result.valid);
    EXPECT_TRUE(result.committed);
    EXPECT_EQ(result.attempts, 3u);
    EXPECT_EQ(result.aborts, 2u);
    EXPECT_EQ(step, 3);
}

TEST(BenchmarkDiagnosticsTest, ExceptionsKeepTheirKindAndPreviousRetries) {
    int step = 0;
    auto allocation = runAttempts(5, [&]() -> AbortCause {
        if (++step == 1) return AbortCause::ReadChanged;
        throw std::bad_alloc();
    });
    EXPECT_EQ(allocation.failure, Failure::AllocationFailure);
    EXPECT_EQ(allocation.attempts, 2u);
    EXPECT_EQ(allocation.aborts, 2u);
    EXPECT_FALSE(allocation.error.empty());
    auto known = runAttempts(5, []() -> AbortCause { throw std::runtime_error("test,\"message\"\nline"); });
    EXPECT_EQ(known.failure, Failure::Exception);
    EXPECT_EQ(known.error, "test,\"message\"\nline");
    auto unknown = runAttempts(5, []() -> AbortCause { throw 7; });
    EXPECT_EQ(unknown.failure, Failure::UnknownException);
    EXPECT_EQ(unknown.attempts, 1u);
    EXPECT_EQ(unknown.aborts, 1u);
}

TEST(BenchmarkDiagnosticsTest, MergeIncludesFailedLogicalTransactionsAndHistogramBounds) {
    PhaseStats first, second;
    auto success = runAttempts(1, [] { return AbortCause::None; });
    success.writes = 8;
    for (int i = 0; i < 99; ++i) first.record(success);
    auto failed = runAttempts(6, [] { return AbortCause::ReadChanged; });
    failed.writes = 999; // Uncommitted writes must not reach the checksum.
    second.record(failed);
    first.merge(second);
    EXPECT_EQ(first.committed, 99u);
    EXPECT_EQ(first.failed, 1u);
    EXPECT_EQ(first.writes, 792u);
    EXPECT_EQ(first.attempts, first.committed + first.aborts);
    EXPECT_EQ(first.retryPercentileUpper(50), 0u);
    EXPECT_EQ(first.retryPercentileUpper(99), 0u);
    EXPECT_EQ(first.retryPercentileUpper(100), 5u);
    EXPECT_EQ(first.max_retries, 5u);
    EXPECT_EQ(PhaseStats{}.retryPercentileUpper(99), 0u);
}

TEST(BenchmarkDiagnosticsTest, BackoffRunsOnlyBetweenAttemptsAndResetsPerLogicalOperation) {
    std::vector<uint64_t> waited;
    auto wait = [&](uint64_t failures) { waited.push_back(failures); };
    auto failed = runAttempts(3, [] { return AbortCause::ReadLocked; }, wait);
    EXPECT_EQ(failed.failure, Failure::RetryLimit);
    EXPECT_EQ(waited, (std::vector<uint64_t>{1, 2}));
    int step = 0;
    auto success = runAttempts(3, [&] {
        return ++step == 1 ? AbortCause::ReadChanged : AbortCause::None;
    }, wait);
    EXPECT_TRUE(success.committed);
    EXPECT_EQ(waited, (std::vector<uint64_t>{1, 2, 1}));
    runAttempts(3, []() -> AbortCause { throw std::runtime_error("stop"); }, wait);
    EXPECT_EQ(waited.size(), 3u);
}

TEST(BenchmarkDiagnosticsTest, JitterDelayIsBoundedEvenForHugeRetryCounts) {
    using STM::Occ::RetryBackoff;
    using STM::Occ::BackoffPolicy;
    RetryBackoff first(BackoffPolicy::Jitter, 123), second(BackoffPolicy::Jitter, 456);
    EXPECT_EQ(RetryBackoff::windowUs(1), 0u);
    EXPECT_EQ(RetryBackoff::windowUs(2), 0u);
    EXPECT_EQ(RetryBackoff::windowUs(3), 1u);
    EXPECT_EQ(RetryBackoff::windowUs(UINT64_MAX), RetryBackoff::kMaxDelayUs);
    bool differs = false, nonzero = false;
    for (unsigned i = 0; i < 100; ++i) {
        const auto a = first.nextDelayUs(UINT64_MAX);
        const auto b = second.nextDelayUs(UINT64_MAX);
        EXPECT_LE(a, RetryBackoff::kMaxDelayUs);
        EXPECT_LE(b, RetryBackoff::kMaxDelayUs);
        differs |= a != b;
        nonzero |= a != 0;
    }
    EXPECT_TRUE(differs);
    EXPECT_TRUE(nonzero);
}
