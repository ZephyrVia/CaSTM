#include <gtest/gtest.h>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

#include "EBRManager/EBRManager.hpp"
#include "WwSTM/Config.hpp"
#include "WwSTM/TMVar.hpp"
#include "WwSTM/TxContext.hpp"
#include "WwSTM/TxDescriptor.hpp"
#include "WwSTM/TxStatus.hpp"

using namespace STM::Ww;

namespace {

// C++17 replacement for a latch.  Each gate is one-way and is only used to
// arrange a specific ownership/lifetime phase; no timing assumption is made.
class PhaseGate {
public:
    void signal() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++phase_;
        }
        condition_.notify_all();
    }

    void wait_until(int target) {
        std::unique_lock<std::mutex> lock(mutex_);
        condition_.wait(lock, [this, target] { return phase_ >= target; });
    }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    int phase_ = 0;
};

struct DescriptorValue {
    int value;
};

using DescriptorTestVar = TMVar<DescriptorValue>;
using DescriptorTestRecord = DescriptorTestVar::RecordT;

void advanceEpochs(EBRManager* manager, int rounds = 8) {
    for (int i = 0; i < rounds; ++i) {
        manager->enter();
        manager->leave();
    }
}

struct HelperAfterOwnerState {
    PhaseGate prepared;
    PhaseGate helper_loaded;
    PhaseGate release_owner;
    PhaseGate allow_helper_dereference;
    PhaseGate helper_dereferenced;

    TxDescriptor* descriptor = nullptr;
    std::atomic<DescriptorTestRecord*> record{nullptr};
    std::atomic<bool> helper_saw_terminal_owner{false};
};

HelperAfterOwnerState* active_helper_after_owner_state = nullptr;

void pauseOwnerAfterPrepare(TxContext& tx) {
    auto* state = active_helper_after_owner_state;
    if (!state) return;

    state->descriptor = tx.debugDescriptorForTest();
    state->prepared.signal();
    state->release_owner.wait_until(1);
}

} // namespace

TEST(TxDescriptorTest, OverAlignedAllocationUsesCacheLineAlignment) {
#if !STM_WW_TEST_HOOKS
    GTEST_SKIP() << "Descriptor lifetime counters require test hooks.";
#else
    const uint64_t allocated_before = TxDescriptor::debugAllocatedCount();
    const uint64_t reclaimed_before = TxDescriptor::debugReclaimedCount();
    const uint64_t destroyed_before = TxDescriptor::debugDestroyedCount();

    constexpr size_t kDescriptorCount = 4096;
    std::vector<TxDescriptor*> descriptors;
    descriptors.reserve(kDescriptorCount);

    for (size_t i = 0; i < kDescriptorCount; ++i) {
        auto* descriptor = new TxDescriptor(static_cast<uint64_t>(i));
        EXPECT_EQ(reinterpret_cast<uintptr_t>(descriptor) % alignof(TxDescriptor),
                  0u);
        EXPECT_EQ(reinterpret_cast<uintptr_t>(descriptor) % 64u, 0u);
        EXPECT_NE(descriptor->debug_tx_id, 0u);
        descriptors.push_back(descriptor);
    }

    for (auto* descriptor : descriptors) {
        delete descriptor;
    }

    EXPECT_EQ(TxDescriptor::debugAllocatedCount() - allocated_before,
              kDescriptorCount);
    EXPECT_EQ(TxDescriptor::debugReclaimedCount() - reclaimed_before,
              kDescriptorCount);
    EXPECT_EQ(TxDescriptor::debugDestroyedCount() - destroyed_before,
              kDescriptorCount);
#endif
}

TEST(TxDescriptorTest, ReclamationWaitsForReaderHoldingPublishedRecord) {
#if STM_WW_VERIFY_LOGIC_MODE
    GTEST_SKIP() << "The descriptor/Record grace-period test needs mode=0 EBR.";
#else
    auto* manager = EBRManager::instance();
    advanceEpochs(manager);

    const uint64_t allocated_before = TxDescriptor::debugAllocatedCount();
    const uint64_t reclaimed_before = TxDescriptor::debugReclaimedCount();
    const uint64_t destroyed_before = TxDescriptor::debugDestroyedCount();
    const uint64_t record_destroyed_before =
        DescriptorTestRecord::debug_destructor_count.load(
            std::memory_order_relaxed);

    // The test thread is the owner/helper (T2) and enters EBR before it
    // publishes, helps, and retires anything.
    manager->enter();
    DescriptorTestVar var(DescriptorValue{0});
    auto* owner = new TxDescriptor(700);
    PhaseGate published;
    PhaseGate reader_loaded;
    PhaseGate allow_reader_dereference;
    PhaseGate reader_dereferenced;
    std::atomic<DescriptorTestRecord*> held_record{nullptr};
    std::atomic<bool> reader_saw_live_owner{false};

    std::thread reader([&] {
        published.wait_until(1);
        manager->enter();

        auto* record = var.debugLoadRecordForTest();
        held_record.store(record, std::memory_order_release);
        reader_loaded.signal();
        allow_reader_dereference.wait_until(1);

        // This is deliberately after T2 has removed/retired the Record and
        // retired the owner descriptor.  The reader is still inside EBR.
        const bool safe = record != nullptr
            && record->owner == owner
            && record->owner->status.load(std::memory_order_acquire)
                   == TxStatus::ABORTED
            && record->owner->debug_tx_id != 0;
        reader_saw_live_owner.store(safe, std::memory_order_release);
        reader_dereferenced.signal();
        manager->leave();
    });

    DescriptorValue draft{7};
    TxDescriptor* conflict = nullptr;
    void* raw_record = var.tryWriteAndGetRecord(owner, &draft, conflict);
    ASSERT_NE(raw_record, nullptr);
    ASSERT_EQ(conflict, nullptr);
    published.signal();
    reader_loaded.wait_until(1);

    owner->status.store(TxStatus::ABORTED, std::memory_order_release);
    ASSERT_TRUE(var.debugHelpCurrentRecordForTest());
    ASSERT_NE(held_record.load(std::memory_order_acquire), nullptr);

    // The Record has been retired by the successful Record -> Node CAS.  The
    // descriptor is retired only now, and must remain unreclaimed while the
    // reader still owns its EBR protection.
    manager->retire(owner);
    EXPECT_EQ(TxDescriptor::debugReclaimedCount() - reclaimed_before, 0u);
    EXPECT_EQ(DescriptorTestRecord::debug_destructor_count.load(
                  std::memory_order_relaxed) - record_destroyed_before,
              0u);
    manager->leave();

    allow_reader_dereference.signal();
    reader_dereferenced.wait_until(1);
    reader.join();

    EXPECT_TRUE(reader_saw_live_owner.load(std::memory_order_acquire));

    // The reader's leave supplies the first possible grace-period transition;
    // these extra enter/leave pairs make the final collection deterministic.
    for (int i = 0;
         i < 8
             && (TxDescriptor::debugReclaimedCount() - reclaimed_before < 1
                 || DescriptorTestRecord::debug_destructor_count.load(
                        std::memory_order_relaxed) - record_destroyed_before < 1);
         ++i) {
        manager->enter();
        manager->leave();
    }

    EXPECT_EQ(TxDescriptor::debugAllocatedCount() - allocated_before, 1u);
    EXPECT_EQ(TxDescriptor::debugReclaimedCount() - reclaimed_before, 1u);
    EXPECT_EQ(TxDescriptor::debugDestroyedCount() - destroyed_before, 1u);
    EXPECT_EQ(DescriptorTestRecord::debug_destructor_count.load(
                  std::memory_order_relaxed) - record_destroyed_before,
              1u);
#endif
}

TEST(TxDescriptorTest, HelperCanReadTerminalOwnerAfterOwnerCleanup) {
#if STM_WW_VERIFY_LOGIC_MODE
    GTEST_SKIP() << "The descriptor/Record grace-period test needs mode=0 EBR.";
#else
    auto* manager = EBRManager::instance();
    advanceEpochs(manager);

    const uint64_t allocated_before = TxDescriptor::debugAllocatedCount();
    const uint64_t reclaimed_before = TxDescriptor::debugReclaimedCount();
    const uint64_t destroyed_before = TxDescriptor::debugDestroyedCount();
    const uint64_t record_destroyed_before =
        DescriptorTestRecord::debug_destructor_count.load(
            std::memory_order_relaxed);

    DescriptorTestVar var(DescriptorValue{0});
    HelperAfterOwnerState state;
    active_helper_after_owner_state = &state;
    TxContext::setPrepareHook(&pauseOwnerAfterPrepare);

    std::thread helper([&] {
        state.prepared.wait_until(1);
        manager->enter();

        auto* record = var.debugLoadRecordForTest();
        state.record.store(record, std::memory_order_release);
        state.helper_loaded.signal();
        state.allow_helper_dereference.wait_until(1);

        // The owner TxContext has already returned by this point.  Its
        // cleanup retired both the published Record and its Descriptor, but
        // this helper's EBR section protects both raw pointers.
        const bool safe = record != nullptr
            && record->owner == state.descriptor
            && record->owner->status.load(std::memory_order_acquire)
                   == TxStatus::COMMITTED
            && record->owner->debug_tx_id != 0;
        state.helper_saw_terminal_owner.store(safe, std::memory_order_release);
        state.helper_dereferenced.signal();
        manager->leave();
    });

    std::atomic<bool> owner_committed{false};
    std::thread owner([&] {
        TxContext tx;
        tx.write(&var, DescriptorValue{42});
        owner_committed.store(tx.commit(), std::memory_order_release);
    });

    state.prepared.wait_until(1);
    state.helper_loaded.wait_until(1);
    state.release_owner.signal();
    owner.join();

    TxContext::clearPrepareHook();
    active_helper_after_owner_state = nullptr;

    ASSERT_TRUE(owner_committed.load(std::memory_order_acquire));
    ASSERT_NE(state.record.load(std::memory_order_acquire), nullptr);
    EXPECT_EQ(TxDescriptor::debugReclaimedCount() - reclaimed_before, 0u);
    EXPECT_EQ(DescriptorTestRecord::debug_destructor_count.load(
                  std::memory_order_relaxed) - record_destroyed_before,
              0u);

    state.allow_helper_dereference.signal();
    state.helper_dereferenced.wait_until(1);
    helper.join();
    EXPECT_TRUE(state.helper_saw_terminal_owner.load(std::memory_order_acquire));

    for (int i = 0;
         i < 8
             && (TxDescriptor::debugReclaimedCount() - reclaimed_before < 1
                 || DescriptorTestRecord::debug_destructor_count.load(
                        std::memory_order_relaxed) - record_destroyed_before < 1);
         ++i) {
        manager->enter();
        manager->leave();
    }

    EXPECT_EQ(TxDescriptor::debugAllocatedCount() - allocated_before, 1u);
    EXPECT_EQ(TxDescriptor::debugReclaimedCount() - reclaimed_before, 1u);
    EXPECT_EQ(TxDescriptor::debugDestroyedCount() - destroyed_before, 1u);
    EXPECT_EQ(DescriptorTestRecord::debug_destructor_count.load(
                  std::memory_order_relaxed) - record_destroyed_before,
              1u);
#endif
}

TEST(TxDescriptorTest, TransactionDescriptorChurnReclaimsThroughEbr) {
#if STM_WW_VERIFY_LOGIC_MODE
    GTEST_SKIP() << "Descriptor churn reclamation needs mode=0 EBR.";
#else
    auto* manager = EBRManager::instance();
    advanceEpochs(manager, 16);

    const uint64_t allocated_before = TxDescriptor::debugAllocatedCount();
    const uint64_t reclaimed_before = TxDescriptor::debugReclaimedCount();
    const uint64_t destroyed_before = TxDescriptor::debugDestroyedCount();

    constexpr size_t kTransactionCount = 2000;
    for (size_t i = 0; i < kTransactionCount; ++i) {
        TxContext tx;
        if ((i & 1u) == 0) {
            ASSERT_TRUE(tx.commit());
        }
        // Odd iterations intentionally leave an ACTIVE transaction to its
        // destructor, covering the descriptor abort-cleanup path as well.
    }

    for (int i = 0;
         i < 16
             && TxDescriptor::debugReclaimedCount() - reclaimed_before
                    < kTransactionCount;
         ++i) {
        manager->enter();
        manager->leave();
    }

    EXPECT_EQ(TxDescriptor::debugAllocatedCount() - allocated_before,
              kTransactionCount);
    EXPECT_EQ(TxDescriptor::debugReclaimedCount() - reclaimed_before,
              kTransactionCount);
    EXPECT_EQ(TxDescriptor::debugDestroyedCount() - destroyed_before,
              kTransactionCount);
#endif
}
