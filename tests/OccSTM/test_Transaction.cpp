#include <gtest/gtest.h>
#include <atomic>
#include <thread>
#include <stdexcept>
#include <vector>

#include "EBRManager/guard.hpp"
#include "OccSTM/Transaction.hpp"

using namespace STM::Occ;

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
