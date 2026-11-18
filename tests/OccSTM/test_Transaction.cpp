#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <stdexcept>
#include <vector>

#include "EBRManager/guard.hpp"
#include "OccSTM/Transaction.hpp"

using namespace STM::Occ;

// Phase-gated production publish callback: first write is visible while a
// second write from the same transaction has not yet been published. The
// original (unlocked-read) implementation accepted the mixed state.
namespace {
struct PartialPublishGate {
    std::atomic<bool> first_published{false};
    std::atomic<bool> resume{false};

    void pauseAfterFirstPublish() noexcept {
        first_published.store(true, std::memory_order_release);
        while (!resume.load(std::memory_order_acquire))
            std::this_thread::yield();
    }

    bool waitForPublication() {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (!first_published.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        return first_published.load(std::memory_order_acquire);
    }

    void releaseWriter() noexcept {
        resume.store(true, std::memory_order_release);
    }
};

std::atomic<PartialPublishGate*> active_publish_gate{nullptr};

void publishFirstThenPause(void* tmvar, void* node, uint64_t wv) noexcept {
    TMVar<int>::committer(tmvar, node, wv);
    auto* gate = active_publish_gate.load(std::memory_order_acquire);
    if (gate) gate->pauseAfterFirstPublish();
}
} // namespace

TEST(OccAtomicVisibilityTest, NeverAcceptsPartiallyPublishedMultiVarWrite) {
    TMVar<int> a(0), b(0);
    PartialPublishGate gate;
    active_publish_gate.store(&gate, std::memory_order_release);
    std::atomic<bool> writer_ok{false};

    std::thread writer([&] {
        ebr::Guard guard(*EBRManager::instance());
        TransactionDescriptor descriptor;
        Transaction tx(&descriptor);
        tx.begin();
        tx.store(a, 1);
        tx.store(b, 1);
        descriptor.writeSet().front().committer = &publishFirstThenPause;
        writer_ok.store(tx.commit(), std::memory_order_release);
    });

    const bool published = gate.waitForPublication();

    bool accepted_mixed = false;
    if (published) {
        ebr::Guard guard(*EBRManager::instance());
        TransactionDescriptor descriptor;
        Transaction tx(&descriptor);
        tx.begin();
        try {
            const int va = tx.load(a);
            const int vb = tx.load(b);
            const bool accepted = tx.commit();
            accepted_mixed = accepted && (va != vb);
        } catch (const RetryException&) {
            // With versioned read barriers, an in-flight writer may cause
            // an immediate retry, which is the correct result.
        }
    }
    gate.releaseWriter();
    writer.join();
    active_publish_gate.store(nullptr, std::memory_order_release);

    EXPECT_TRUE(published);
    EXPECT_TRUE(writer_ok.load(std::memory_order_acquire));
    EXPECT_FALSE(accepted_mixed)
        << "a read-only transaction accepted A=1, B=0 mid-commit";

    ebr::Guard guard(*EBRManager::instance());
    TransactionDescriptor descriptor;
    Transaction tx(&descriptor);
    tx.begin();
    EXPECT_EQ(tx.load(a), 1);
    EXPECT_EQ(tx.load(b), 1);
    EXPECT_TRUE(tx.commit());
}

// A writer that already took WV for A but has not completed publication
// cannot globally serialize an independent writer on a different stripe.
// If the second writer finishes first, a reader with RV >= both WVs must
// still reject A while its older-WV writer is holding the version lock.
TEST(OccAtomicVisibilityTest, DisjointWritersMayCommitOutOfTimestampOrder) {
    TMVar<int> a(0), b(0);
    auto& stripes = StripedLockTable::instance();
    ASSERT_NE(stripes.getStripeIndex(&a), stripes.getStripeIndex(&b));

    PartialPublishGate gate;
    active_publish_gate.store(&gate, std::memory_order_release);
    std::atomic<bool> a_committed{false}, b_committed{false};

    std::thread writer_a([&] {
        ebr::Guard guard(*EBRManager::instance());
        TransactionDescriptor descriptor;
        Transaction tx(&descriptor);
        tx.begin();
        tx.store(a, 1);
        descriptor.writeSet().front().committer = &publishFirstThenPause;
        a_committed.store(tx.commit(), std::memory_order_release);
    });

    const bool a_is_published = gate.waitForPublication();
    std::thread writer_b([&] {
        ebr::Guard guard(*EBRManager::instance());
        TransactionDescriptor descriptor;
        Transaction tx(&descriptor);
        tx.begin();
        tx.store(b, 1);
        b_committed.store(tx.commit(), std::memory_order_release);
    });

    // Bound the wait so a regression adding a global commit lock does not
    // hang the test indefinitely.
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!b_committed.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    const bool b_finished_first = b_committed.load(std::memory_order_acquire);

    bool old_timestamp_snapshot_was_accepted = false;
    bool saw_pending_writer = false;
    if (a_is_published && b_finished_first) {
        ebr::Guard guard(*EBRManager::instance());
        TransactionDescriptor descriptor;
        Transaction tx(&descriptor);
        tx.begin(); // RV includes B's WV and A's still-in-flight WV.
        try {
            const int va = tx.load(a);
            const int vb = tx.load(b);
            old_timestamp_snapshot_was_accepted =
                tx.commit() && va != 1 && vb == 1;
        } catch (const RetryException&) {
            saw_pending_writer = true;
        }
    }

    gate.releaseWriter();
    writer_a.join();
    writer_b.join();
    active_publish_gate.store(nullptr, std::memory_order_release);

    EXPECT_TRUE(a_is_published);
    EXPECT_TRUE(b_finished_first)
        << "a disjoint stripe must not wait for another writer's commit";
    EXPECT_TRUE(a_committed.load(std::memory_order_acquire));
    EXPECT_TRUE(b_committed.load(std::memory_order_acquire));
    EXPECT_TRUE(saw_pending_writer)
        << "RV including an unfinished WV must not accept a partial state";
    EXPECT_FALSE(old_timestamp_snapshot_was_accepted);
    EXPECT_LT(a.loadHead()->write_ts, b.loadHead()->write_ts);

    ebr::Guard guard(*EBRManager::instance());
    TransactionDescriptor descriptor;
    Transaction tx(&descriptor);
    tx.begin();
    EXPECT_EQ(tx.load(a), 1);
    EXPECT_EQ(tx.load(b), 1);
    EXPECT_TRUE(tx.commit());
}

// Each successful read-only snapshot should observe a consistent pair from
// a multi-variable updater. RetryException is allowed if a version lock
// overlaps a read or bounded history no longer contains its RV.
TEST(OccAtomicVisibilityTest, ConcurrentHistoricalSnapshotPairsRemainConsistent) {
    TMVar<int> a(0), b(0);
    constexpr int kCommits = 800;
    constexpr int kReaders = 4;
    constexpr int kReadsPerThread = 600;
    std::atomic<bool> start{false};
    std::atomic<int> bad_pairs{0}, accepted{0}, retries{0};
    std::vector<std::thread> threads;

    threads.emplace_back([&] {
        ebr::Guard guard(*EBRManager::instance());
        TransactionDescriptor descriptor;
        Transaction tx(&descriptor);
        while (!start.load(std::memory_order_acquire))
            std::this_thread::yield();
        for (int value = 1; value <= kCommits; ++value) {
            tx.begin();
            tx.store(a, value);
            tx.store(b, value);
            if (!tx.commit()) ++bad_pairs;
        }
    });

    for (int i = 0; i < kReaders; ++i) {
        threads.emplace_back([&] {
            ebr::Guard guard(*EBRManager::instance());
            TransactionDescriptor descriptor;
            Transaction tx(&descriptor);
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();
            for (int attempt = 0; attempt < kReadsPerThread; ++attempt) {
                tx.begin();
                try {
                    const int va = tx.load(a);
                    const int vb = tx.load(b);
                    if (tx.commit()) {
                        ++accepted;
                        if (va != vb) ++bad_pairs;
                    }
                } catch (const RetryException&) {
                    ++retries;
                }
            }
        });
    }

    start.store(true, std::memory_order_release);
    for (auto& thread : threads) thread.join();

    EXPECT_EQ(bad_pairs.load(), 0);
    EXPECT_EQ(accepted.load() + retries.load(),
              kReaders * kReadsPerThread);
    EXPECT_EQ(a.loadHead()->payload, kCommits);
    EXPECT_EQ(b.loadHead()->payload, kCommits);
}

class OccTransactionTest : public ::testing::Test {
protected:
    void SetUp() override { EBRManager::instance()->enter(); }
    void TearDown() override {
        desc.reset();
        EBRManager::instance()->leave();
    }
    TransactionDescriptor desc;
};

TEST_F(OccTransactionTest, BasicStoreAndCommit) {
    TMVar<int> var(10);
    Transaction tx(&desc);
    tx.begin();
    tx.store(var, 20);
    EXPECT_TRUE(tx.commit());
    EXPECT_EQ(var.loadHead()->payload, 20);
    EXPECT_GT(var.loadHead()->write_ts, 0u);
    EXPECT_EQ(tx.lastAbortReason(), AbortReason::None);
}

TEST_F(OccTransactionTest, ReadYourOwnWrites) {
    TMVar<int> var(10);
    Transaction tx(&desc);
    tx.begin();
    EXPECT_EQ(tx.load(var), 10);
    tx.store(var, 20);
    EXPECT_EQ(tx.load(var), 20);
    tx.store(var, 30);
    EXPECT_EQ(tx.load(var), 30);
    EXPECT_TRUE(tx.commit());
    EXPECT_EQ(var.loadHead()->payload, 30);
}

// Covers a retained single-variable snapshot, not a general isolation proof.
TEST_F(OccTransactionTest, ReadsRetainedVersionAfterAnotherCommit) {
    TMVar<int> var(100);
    Transaction reader(&desc);
    TransactionDescriptor writer_desc;
    Transaction writer(&writer_desc);
    reader.begin();
    writer.begin();
    writer.store(var, 200);
    ASSERT_TRUE(writer.commit());
    EXPECT_EQ(reader.load(var), 100);
    EXPECT_TRUE(reader.commit());
}

TEST_F(OccTransactionTest, ReadOnlyCommitAfterHeadChanges) {
    TMVar<int> var(10);
    Transaction reader(&desc);
    TransactionDescriptor writer_desc;
    Transaction writer(&writer_desc);
    reader.begin();
    EXPECT_EQ(reader.load(var), 10);
    writer.begin();
    writer.store(var, 11);
    ASSERT_TRUE(writer.commit());
    EXPECT_TRUE(reader.commit());
    EXPECT_EQ(var.loadHead()->payload, 11);
}

TEST_F(OccTransactionTest, ChangedReadAbortsWithoutPublishingAndCanRetry) {
    TMVar<int> x(10), y(20);
    Transaction tx(&desc);
    TransactionDescriptor other_desc;
    Transaction other(&other_desc);
    tx.begin();
    EXPECT_EQ(tx.load(x), 10);
    tx.store(y, 99);
    other.begin();
    other.store(x, 11);
    ASSERT_TRUE(other.commit());
    EXPECT_FALSE(tx.commit());
    EXPECT_EQ(tx.lastAbortReason(), AbortReason::ReadVersionChanged);
    EXPECT_EQ(y.loadHead()->payload, 20);
    EXPECT_TRUE(desc.lockSet().empty());
    EXPECT_FALSE(StripedLockTable::instance().is_locked(&y));

    tx.begin(); // Failed draft and failure reason must not leak into the retry.
    EXPECT_EQ(tx.lastAbortReason(), AbortReason::None);
    EXPECT_TRUE(desc.writeSet().empty());
    EXPECT_TRUE(desc.readSet().empty());
    EXPECT_EQ(tx.load(y), 20);
    tx.store(y, tx.load(x) + 20);
    ASSERT_TRUE(tx.commit());
    EXPECT_EQ(y.loadHead()->payload, 31);
}

TEST_F(OccTransactionTest, LockedReadReportsConflictAndReleasesOwnLocks) {
    TMVar<int> x(10), y(20);
    auto& locks = StripedLockTable::instance();
    ASSERT_NE(locks.getStripeIndex(&x), locks.getStripeIndex(&y));
    Transaction tx(&desc);
    tx.begin();
    tx.load(x);
    tx.store(y, 21);
    struct ScopedLock {
        TMVar<int>& var;
        explicit ScopedLock(TMVar<int>& v) : var(v) { StripedLockTable::instance().lock(&var); }
        ~ScopedLock() { StripedLockTable::instance().unlock(&var); }
    } held(x);
    EXPECT_FALSE(tx.commit());
    EXPECT_EQ(tx.lastAbortReason(), AbortReason::ReadLocked);
    EXPECT_EQ(y.loadHead()->payload, 20);
    EXPECT_TRUE(desc.lockSet().empty());
    EXPECT_FALSE(locks.is_locked(&y));
}

TEST_F(OccTransactionTest, OwnWriteLockDoesNotCauseReadConflict) {
    TMVar<int> x(10);
    Transaction tx(&desc);
    tx.begin();
    tx.store(x, tx.load(x) + 1);
    EXPECT_TRUE(tx.commit());
    EXPECT_EQ(tx.lastAbortReason(), AbortReason::None);
    EXPECT_EQ(x.loadHead()->payload, 11);
}

TEST_F(OccTransactionTest, PrunedHistoryReportsSnapshotUnavailableAndCanRetry) {
    TMVar<int> var(0);
    Transaction reader(&desc);
    TransactionDescriptor writer_desc;
    Transaction writer(&writer_desc);
    reader.begin();
    for (int i = 1; i <= 10; ++i) {
        writer.begin();
        writer.store(var, i);
        ASSERT_TRUE(writer.commit());
    }
    EXPECT_THROW(reader.load(var), RetryException);
    EXPECT_EQ(reader.lastAbortReason(), AbortReason::SnapshotUnavailable);
    reader.begin();
    EXPECT_EQ(reader.lastAbortReason(), AbortReason::None);
    EXPECT_EQ(reader.load(var), 10);
    EXPECT_TRUE(reader.commit());
}

TEST(OccConcurrencyTest, BoundedConcurrentIncrementsPreserveCommittedSum) {
    TMVar<int> counter(0);
    constexpr int thread_count = 4;
    constexpr int increments = 200;
    std::atomic<bool> start{false};
    std::atomic<int> completed{0}, failures{0};
    std::vector<std::thread> workers;
    for (int t = 0; t < thread_count; ++t) {
        workers.emplace_back([&] {
            TransactionDescriptor desc;
            Transaction tx(&desc);
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            for (int i = 0; i < increments; ++i) {
                ebr::Guard guard(*EBRManager::instance());
                bool committed = false;
                for (int attempt = 0; attempt < 100000; ++attempt) {
                    tx.begin();
                    try { tx.store(counter, tx.load(counter) + 1); }
                    catch (const RetryException&) { continue; }
                    if (tx.commit()) { committed = true; break; }
                    std::this_thread::yield();
                }
                if (!committed) { ++failures; return; }
                ++completed;
            }
        });
    }
    start.store(true, std::memory_order_release);
    for (auto& worker : workers) worker.join();
    EXPECT_EQ(failures.load(), 0);
    EXPECT_EQ(completed.load(), thread_count * increments);
    EXPECT_EQ(counter.loadHead()->payload, completed.load());
}

#include "OccSTM/STM.hpp"

TEST(OccAtomicApiTest, RetriesChangedReadAndReturnsOnlySuccessfulAttemptValue) {
    STM::Var<int> x(10), y(0);
    TransactionDescriptor writer_desc;
    Transaction writer(&writer_desc);
    int calls = 0;
    const int value = STM::atomically([&](Transaction& tx) {
        const int observed = tx.load(x);
        if (++calls == 1) {
            writer.begin();
            writer.store(x, 11);
            EXPECT_TRUE(writer.commit());
        }
        tx.store(y, observed);
        return observed;
    });
    EXPECT_EQ(calls, 2);
    EXPECT_EQ(value, 11);
    EXPECT_EQ(y.loadHead()->payload, 11);
}

TEST(OccAtomicApiTest, VoidCallbackAndExceptionLeaveNextTransactionUsable) {
    STM::Var<int> var(0);
    EXPECT_THROW(STM::atomically([&](Transaction& tx) {
        tx.store(var, 99);
        throw std::runtime_error("user callback");
    }), std::runtime_error);
    EXPECT_EQ(var.loadHead()->payload, 0);
    STM::atomically([&](Transaction& tx) { tx.store(var, tx.load(var) + 1); });
    EXPECT_EQ(var.loadHead()->payload, 1);
}
