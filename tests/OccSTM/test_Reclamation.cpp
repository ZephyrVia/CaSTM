#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

#include "EBRManager/guard.hpp"
#include "OccSTM/Transaction.hpp"

namespace {
using namespace STM::Occ;

// Gates constrain the lifetime cases without relying on sleeps. The stress
// case below deliberately leaves load/commit unordered so TSan can find races.
class Gate {
public:
    void open() { open_.store(true, std::memory_order_release); }
    bool wait() {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!open_.load(std::memory_order_acquire)) {
            if (std::chrono::steady_clock::now() >= deadline) return false;
            std::this_thread::yield();
        }
        return true;
    }
private:
    std::atomic<bool> open_{false};
};

struct Destructions {
    std::array<std::atomic<unsigned>, 64> by_generation;
    Destructions() {
        for (auto& count : by_generation) count.store(0, std::memory_order_relaxed);
    }
    unsigned count(int generation) const {
        return by_generation[generation].load(std::memory_order_relaxed);
    }
};

struct Probe {
    std::shared_ptr<Destructions> counts;
    int generation;
    Probe(std::shared_ptr<Destructions> counts, int generation)
        : counts(std::move(counts)), generation(generation) {}
    ~Probe() { counts->by_generation[generation].fetch_add(1, std::memory_order_relaxed); }
};
using ProbeVar = TMVar<std::shared_ptr<Probe>>;

// Keep the test source usable on the pre-fix raw-pointer implementation too.
// Production traversal uses explicit acquire loads after the fix.
ProbeVar::Node* previous(ProbeVar::Node* node) { return node->prev; }

void drain_epochs() {
    for (int i = 0; i < 16; ++i) {
        ebr::Guard guard(*EBRManager::instance());
    }
}

bool publish(ProbeVar& var, const std::shared_ptr<Destructions>& counts, int generation) {
    TransactionDescriptor descriptor;
    Transaction transaction(&descriptor);
    ebr::Guard guard(*EBRManager::instance());
    transaction.begin();
    transaction.store(var, std::make_shared<Probe>(counts, generation));
    return transaction.commit();
}

TEST(OccReclamationTest, ReaderObservesCutEdgeAndRetriesWithFreshSnapshot) {
    auto counts = std::make_shared<Destructions>();
    {
        ProbeVar var(std::make_shared<Probe>(counts, 0));
        for (int n = 1; n <= ProbeVar::MAX_HISTORY; ++n) ASSERT_TRUE(publish(var, counts, n));
        Gate reader_ready, allow_edge_read;
        bool edge_was_cut = false, reader_released = false;
        std::thread reader([&] {
            ebr::Guard guard(*EBRManager::instance());
            auto* parent = var.loadHead();
            for (int i = 0; i < ProbeVar::MAX_HISTORY - 1 && parent; ++i) parent = previous(parent);
            reader_ready.open();
            reader_released = allow_edge_read.wait();
            edge_was_cut = parent && previous(parent) == nullptr;
        });
        const bool ready = reader_ready.wait();
        EXPECT_TRUE(ready);
        if (ready) {
            EXPECT_TRUE(publish(var, counts, ProbeVar::MAX_HISTORY + 1));
            drain_epochs();
            EXPECT_EQ(counts->count(0), 0u) << "the reader still protects the detached node";
        }
        allow_edge_read.open();
        reader.join();
        EXPECT_TRUE(reader_released);
        EXPECT_TRUE(edge_was_cut);
        drain_epochs();
        EXPECT_EQ(counts->count(0), 1u);

        TransactionDescriptor descriptor;
        Transaction transaction(&descriptor);
        ebr::Guard guard(*EBRManager::instance());
        transaction.begin();
        descriptor.setReadVersion(0);
        EXPECT_THROW(transaction.load(var), RetryException);
        EXPECT_EQ(transaction.lastAbortReason(), AbortReason::SnapshotUnavailable);
        transaction.begin();
        EXPECT_EQ(transaction.load(var)->generation, ProbeVar::MAX_HISTORY + 1);
        EXPECT_TRUE(transaction.commit());
    }
    drain_epochs();
    for (int n = 0; n <= ProbeVar::MAX_HISTORY + 1; ++n) EXPECT_EQ(counts->count(n), 1u) << n;
}

TEST(OccReclamationTest, CachedTailSurvivesRepeatedPruningUntilReaderLeaves) {
    constexpr int last_generation = 24;
    auto counts = std::make_shared<Destructions>();
    {
        ProbeVar var(std::make_shared<Probe>(counts, 0));
        for (int n = 1; n <= ProbeVar::MAX_HISTORY; ++n) ASSERT_TRUE(publish(var, counts, n));
        Gate reader_ready, allow_payload_read;
        int observed_generation = -1;
        bool reader_released = false;
        std::thread reader([&] {
            ebr::Guard guard(*EBRManager::instance());
            auto* cached = var.loadHead();
            for (int i = 0; i < ProbeVar::MAX_HISTORY && cached; ++i) cached = previous(cached);
            // No shared_ptr copy: lifetime protection must come from EBR.
            reader_ready.open();
            reader_released = allow_payload_read.wait();
            if (cached) observed_generation = cached->payload->generation;
        });
        const bool ready = reader_ready.wait();
        EXPECT_TRUE(ready);
        if (ready) {
            for (int n = ProbeVar::MAX_HISTORY + 1; n <= last_generation; ++n)
                EXPECT_TRUE(publish(var, counts, n));
            drain_epochs();
            for (int n = 0; n <= last_generation; ++n) EXPECT_EQ(counts->count(n), 0u) << n;
        }
        allow_payload_read.open();
        reader.join();
        EXPECT_TRUE(reader_released);
        EXPECT_EQ(observed_generation, 0);
        drain_epochs();
        for (int n = 0; n <= last_generation; ++n)
            EXPECT_EQ(counts->count(n), n < last_generation - ProbeVar::MAX_HISTORY ? 1u : 0u) << n;
    }
    drain_epochs();
    for (int n = 0; n <= last_generation; ++n) EXPECT_EQ(counts->count(n), 1u) << n;
}

TEST(OccReclamationTest, MultipleWritesInOneCommitRetireEachNodeOnce) {
    constexpr int last_generation = 32;
    auto counts = std::make_shared<Destructions>();
    {
        ProbeVar var(std::make_shared<Probe>(counts, 0));
        {
            ebr::Guard guard(*EBRManager::instance());
            TransactionDescriptor descriptor;
            Transaction transaction(&descriptor);
            transaction.begin();
            for (int n = 1; n <= last_generation; ++n)
                transaction.store(var, std::make_shared<Probe>(counts, n));
            EXPECT_EQ(transaction.load(var)->generation, last_generation);
            ASSERT_TRUE(transaction.commit());
            for (int n = 0; n <= last_generation; ++n) EXPECT_EQ(counts->count(n), 0u) << n;
        }
        drain_epochs();
        for (int n = 0; n <= last_generation; ++n)
            EXPECT_EQ(counts->count(n), n < last_generation - ProbeVar::MAX_HISTORY ? 1u : 0u) << n;
        EXPECT_EQ(var.loadHead()->payload->generation, last_generation);
    }
    drain_epochs();
    for (int n = 0; n <= last_generation; ++n) EXPECT_EQ(counts->count(n), 1u) << n;
}

// The reader retains a published parent under EBR rather than acquiring a
// newer head on each iteration. The relaxed stop flag deliberately does not
// order the link read after the writer's cut, exposing the original raw-link
// race to TSan. Both old-pointer and null observations are legal after the fix.
TEST(OccReclamationTest, PublishedEdgeReadOverlapsPruning) {
    auto counts = std::make_shared<Destructions>();
    {
        ProbeVar var(std::make_shared<Probe>(counts, 0));
        for (int n = 1; n <= ProbeVar::MAX_HISTORY; ++n) ASSERT_TRUE(publish(var, counts, n));
        Gate reader_ready;
        std::atomic<bool> cut_done{false};
        unsigned invalid = 0, reads = 0;
        std::thread reader([&] {
            ebr::Guard guard(*EBRManager::instance());
            auto* parent = var.loadHead();
            for (int i = 0; i < ProbeVar::MAX_HISTORY - 1 && parent; ++i) parent = previous(parent);
            reader_ready.open();
            do {
                if (!parent) { ++invalid; break; }
                auto* next = previous(parent);
                if (next && next->payload->generation != 0) ++invalid;
                ++reads;
                std::this_thread::yield();
            } while (!cut_done.load(std::memory_order_relaxed));
        });
        EXPECT_TRUE(reader_ready.wait());
        EXPECT_TRUE(publish(var, counts, ProbeVar::MAX_HISTORY + 1));
        cut_done.store(true, std::memory_order_relaxed);
        reader.join();
        EXPECT_GT(reads, 0u);
        EXPECT_EQ(invalid, 0u);
        drain_epochs();
        EXPECT_EQ(counts->count(0), 1u);
    }
    drain_epochs();
    for (int n = 0; n <= ProbeVar::MAX_HISTORY + 1; ++n) EXPECT_EQ(counts->count(n), 1u) << n;
}

TEST(OccReclamationTest, ConcurrentSnapshotTraversalAndPruning) {
    TMVar<int> var(0);
    constexpr int iterations = 4000;
    constexpr int reader_count = 4;
    std::atomic<bool> start{false};
    std::atomic<unsigned> bad_values{0}, attempts{0}, failed_writes{0};
    std::vector<std::thread> readers;
    for (int i = 0; i < reader_count; ++i) {
        readers.emplace_back([&] {
            TransactionDescriptor descriptor;
            Transaction transaction(&descriptor);
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            for (int n = 0; n < iterations; ++n) {
                ebr::Guard guard(*EBRManager::instance());
                transaction.begin();
                descriptor.setReadVersion(0); // Always traverse toward the oldest version.
                try {
                    if (transaction.load(var) != 0) ++bad_values;
                    if (!transaction.commit()) ++bad_values;
                } catch (const RetryException&) {
                    if (transaction.lastAbortReason() != AbortReason::SnapshotUnavailable) ++bad_values;
                }
                attempts.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    std::thread writer([&] {
        TransactionDescriptor descriptor;
        Transaction transaction(&descriptor);
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        for (int n = 1; n <= iterations; ++n) {
            ebr::Guard guard(*EBRManager::instance());
            transaction.begin();
            transaction.store(var, n);
            if (!transaction.commit()) ++failed_writes;
        }
    });
    start.store(true, std::memory_order_release);
    writer.join();
    for (auto& reader : readers) reader.join();
    drain_epochs();
    EXPECT_EQ(bad_values.load(), 0u);
    EXPECT_EQ(failed_writes.load(), 0u);
    EXPECT_EQ(attempts.load(), reader_count * iterations);
    EXPECT_EQ(var.loadHead()->payload, iterations);
}
} // namespace
