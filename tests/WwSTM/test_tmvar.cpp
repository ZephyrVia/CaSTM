#include <gtest/gtest.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "WwSTM/TMVar.hpp"
#include "WwSTM/TxDescriptor.hpp"
#include "WwSTM/TxStatus.hpp"
#include "EBRManager/EBRManager.hpp"

using namespace STM::Ww;

namespace {

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

struct LifetimeValue {
    int value;
};

struct PublishedRecordState {
    PhaseGate published;
    PhaseGate reader_loaded;
    PhaseGate allow_reader_dereference;
    PhaseGate reader_dereferenced;
    std::atomic<bool> reader_saw_live_record{false};
};

struct HeadChangeState {
    TxDescriptor* writer = nullptr;
    std::atomic<bool> first_hook{true};
};

HeadChangeState* active_head_change_state = nullptr;

void changeHeadDuringRead(TMVar<int>& var) {
    auto* state = active_head_change_state;
    if (!state || !state->first_hook.exchange(false, std::memory_order_acq_rel)) {
        return;
    }

    state->writer->status.store(TxStatus::ABORTED, std::memory_order_release);
    var.debugHelpCurrentRecordForTest();
}

} // namespace

class TMVarTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Direct TMVar tests dereference raw Node/Record pointers.  Keep the
        // entire test body inside one EBR critical section in mode=0.
        EBRManager::instance()->enter();
    }

    void TearDown() override {
        EBRManager::instance()->leave();
    }
};

TEST_F(TMVarTest, Construction) {
    TMVar<int> var(100);
    TxDescriptor tx(1);

    ASSERT_EQ(var.readProxy(&tx), 100);
    ASSERT_EQ(var.getDataVersion(), 0);
}

TEST_F(TMVarTest, StableNodeReadSnapshot) {
    TMVar<int> var(10);
    TxDescriptor tx(1);

    auto snapshot = var.readSnapshot(&tx);
    ASSERT_EQ(snapshot.value, 10);
    ASSERT_EQ(snapshot.version, 0);
    ASSERT_NE(snapshot.node, nullptr);
}

TEST_F(TMVarTest, ReadYourOwnWrites) {
    TMVar<int> var(10);
    TxDescriptor tx(100);
    int write_val = 20;
    TxDescriptor* conflict = nullptr;

    auto* rec = var.tryWriteAndGetRecord(&tx, &write_val, conflict);
    ASSERT_NE(rec, nullptr);
    ASSERT_EQ(conflict, nullptr);
    ASSERT_EQ(var.readProxy(&tx), 20);

    tx.status.store(TxStatus::COMMITTED, std::memory_order_release);
    ASSERT_TRUE(var.commitReleaseRecord(105));

    TxDescriptor tx2(200);
    ASSERT_EQ(var.readProxy(&tx2), 20);
    ASSERT_EQ(var.getDataVersion(), 105);
}

TEST_F(TMVarTest, IsolationSnapshotRead) {
    TMVar<int> var(100);
    TxDescriptor tx_writer(10);
    TxDescriptor tx_reader(20);
    int val = 200;
    TxDescriptor* conflict = nullptr;

    ASSERT_NE(var.tryWriteAndGetRecord(&tx_writer, &val, conflict), nullptr);
    ASSERT_EQ(var.readProxy(&tx_reader), 100);

    tx_writer.status.store(TxStatus::COMMITTED, std::memory_order_release);
    ASSERT_TRUE(var.commitReleaseRecord(15));
    ASSERT_EQ(var.readProxy(&tx_reader), 200);
}

TEST_F(TMVarTest, WriteWriteConflict) {
    TMVar<int> var(10);
    TxDescriptor tx1(100);
    TxDescriptor tx2(200);
    int val1 = 20;
    int val2 = 30;
    TxDescriptor* conflict = nullptr;

    ASSERT_NE(var.tryWriteAndGetRecord(&tx1, &val1, conflict), nullptr);
    conflict = nullptr;
    ASSERT_EQ(var.tryWriteAndGetRecord(&tx2, &val2, conflict), nullptr);
    ASSERT_EQ(conflict, &tx1);

    tx1.status.store(TxStatus::COMMITTED, std::memory_order_release);
    ASSERT_TRUE(var.commitReleaseRecord(110));
}

TEST_F(TMVarTest, ReentrantWriteReusesOneLocatorAndDraft) {
    TMVar<int> var(0);
    TxDescriptor tx(50);
    TxDescriptor* conflict = nullptr;
    int v1 = 1;
    int v2 = 2;
    int v3 = 3;

    auto* first = var.tryWriteAndGetRecord(&tx, &v1, conflict);
    ASSERT_NE(first, nullptr);
    auto* first_record = static_cast<typename TMVar<int>::RecordT*>(first);
    auto* first_node = first_record->new_node;

    ASSERT_EQ(var.tryWriteAndGetRecord(&tx, &v2, conflict), first);
    ASSERT_EQ(var.tryWriteAndGetRecord(&tx, &v3, conflict), first);
    ASSERT_EQ(var.debugLoadRecordForTest(), first_record);
    ASSERT_EQ(first_record->new_node, first_node);
    ASSERT_EQ(first_record->new_node->payload, 3);
    ASSERT_EQ(var.readProxy(&tx), 3);

    tx.status.store(TxStatus::COMMITTED, std::memory_order_release);
    ASSERT_TRUE(var.commitReleaseRecord(60));

    TxDescriptor tx_check(70);
    ASSERT_EQ(var.readProxy(&tx_check), 3);
}

TEST_F(TMVarTest, AbortAndRollback) {
    TMVar<int> var(50);
    TxDescriptor tx(100);
    int val = 99;
    TxDescriptor* conflict = nullptr;

    void* rec_ptr = var.tryWriteAndGetRecord(&tx, &val, conflict);
    ASSERT_NE(rec_ptr, nullptr);
    ASSERT_EQ(var.readProxy(&tx), 99);

    tx.status.store(TxStatus::ABORTED, std::memory_order_release);
    var.abortRestoreData(rec_ptr);

    TxDescriptor tx2(200);
    ASSERT_EQ(var.readProxy(&tx2), 50);
    int val2 = 60;
    ASSERT_NE(var.tryWriteAndGetRecord(&tx2, &val2, conflict), nullptr);
    tx2.status.store(TxStatus::COMMITTED, std::memory_order_release);
    ASSERT_TRUE(var.commitReleaseRecord(210));
}

TEST_F(TMVarTest, AbortedRecordReadSnapshotUsesOldNode) {
    TMVar<int> var(10);
    TxDescriptor writer(100);
    TxDescriptor reader(200);
    int draft = 20;
    TxDescriptor* conflict = nullptr;

    auto* raw_record = var.tryWriteAndGetRecord(&writer, &draft, conflict);
    ASSERT_NE(raw_record, nullptr);
    auto* record = static_cast<typename TMVar<int>::RecordT*>(raw_record);
    writer.status.store(TxStatus::ABORTED, std::memory_order_release);

    auto snapshot = var.readSnapshot(&reader);
    ASSERT_EQ(snapshot.value, 10);
    ASSERT_EQ(snapshot.version, record->old_node->loadWriteTs());
    ASSERT_EQ(snapshot.node, record->old_node);
    ASSERT_TRUE(var.debugHelpCurrentRecordForTest());
}

TEST_F(TMVarTest, ActiveRecordReadSnapshotUsesOldNode) {
    TMVar<int> var(10);
    TxDescriptor writer(100);
    TxDescriptor reader(200);
    int draft = 20;
    TxDescriptor* conflict = nullptr;

    auto* raw_record = var.tryWriteAndGetRecord(&writer, &draft, conflict);
    ASSERT_NE(raw_record, nullptr);
    auto* record = static_cast<typename TMVar<int>::RecordT*>(raw_record);

    auto snapshot = var.readSnapshot(&reader);
    ASSERT_EQ(snapshot.value, 10);
    ASSERT_EQ(snapshot.version, record->old_node->loadWriteTs());
    ASSERT_EQ(snapshot.node, record->old_node);

    writer.status.store(TxStatus::ABORTED, std::memory_order_release);
    ASSERT_TRUE(var.debugHelpCurrentRecordForTest());
}

TEST_F(TMVarTest, CommittedRecordReadSnapshotUsesNewNodeBeforeFlatten) {
    TMVar<int> var(10);
    TxDescriptor writer(100);
    TxDescriptor reader(200);
    int draft = 20;
    TxDescriptor* conflict = nullptr;

    auto* raw_record = var.tryWriteAndGetRecord(&writer, &draft, conflict);
    ASSERT_NE(raw_record, nullptr);
    auto* record = static_cast<typename TMVar<int>::RecordT*>(raw_record);
    record->new_node->storeWriteTs(15);
    writer.status.store(TxStatus::COMMITTED, std::memory_order_release);

    ASSERT_EQ(var.debugLoadRecordForTest(), record);
    auto snapshot = var.readSnapshot(&reader);
    ASSERT_EQ(snapshot.value, 20);
    ASSERT_EQ(snapshot.version, 15);
    ASSERT_EQ(snapshot.node, record->new_node);
    ASSERT_EQ(var.debugLoadRecordForTest(), record);

    ASSERT_TRUE(var.debugHelpCurrentRecordForTest());
    ASSERT_EQ(var.debugLoadNodeForTest(), record->new_node);
}

TEST_F(TMVarTest, HeadIdentityChangeRetriesSnapshot) {
#if STM_WW_VERIFY_LOGIC_MODE
    GTEST_SKIP() << "The deterministic head hook is for the CAS/EBR path.";
#else
    TMVar<int> var(10);
    TxDescriptor writer(100);
    TxDescriptor reader(200);
    int draft = 20;
    TxDescriptor* conflict = nullptr;

    auto* raw_record = var.tryWriteAndGetRecord(&writer, &draft, conflict);
    ASSERT_NE(raw_record, nullptr);
    auto* record = static_cast<typename TMVar<int>::RecordT*>(raw_record);

    HeadChangeState state;
    state.writer = &writer;
    active_head_change_state = &state;
    TMVar<int>::setReadSnapshotHook(&changeHeadDuringRead);
    auto snapshot = var.readSnapshot(&reader);
    TMVar<int>::clearReadSnapshotHook();
    active_head_change_state = nullptr;

    ASSERT_EQ(snapshot.value, 10);
    ASSERT_EQ(snapshot.node, record->old_node);
    ASSERT_EQ(var.debugLoadRecordForTest(), nullptr);
#endif
}

TEST_F(TMVarTest, HelpingFlattensAbortedAndCommittedLocators) {
    TMVar<int> aborted_var(10);
    TxDescriptor aborted_writer(100);
    int aborted_draft = 20;
    TxDescriptor* conflict = nullptr;
    auto* aborted_raw = aborted_var.tryWriteAndGetRecord(
        &aborted_writer, &aborted_draft, conflict);
    ASSERT_NE(aborted_raw, nullptr);
    auto* aborted_record = static_cast<typename TMVar<int>::RecordT*>(aborted_raw);
    auto* aborted_old = aborted_record->old_node;
    aborted_writer.status.store(TxStatus::ABORTED, std::memory_order_release);
    ASSERT_TRUE(aborted_var.debugHelpCurrentRecordForTest());
    ASSERT_EQ(aborted_var.debugLoadRecordForTest(), nullptr);
    ASSERT_EQ(aborted_var.debugLoadNodeForTest(), aborted_old);

    TMVar<int> committed_var(10);
    TxDescriptor committed_writer(200);
    int committed_draft = 30;
    auto* committed_raw = committed_var.tryWriteAndGetRecord(
        &committed_writer, &committed_draft, conflict);
    ASSERT_NE(committed_raw, nullptr);
    auto* committed_record = static_cast<typename TMVar<int>::RecordT*>(committed_raw);
    auto* committed_new = committed_record->new_node;
    committed_new->storeWriteTs(25);
    committed_writer.status.store(TxStatus::COMMITTED, std::memory_order_release);
    ASSERT_TRUE(committed_var.debugHelpCurrentRecordForTest());
    ASSERT_EQ(committed_var.debugLoadRecordForTest(), nullptr);
    ASSERT_EQ(committed_var.debugLoadNodeForTest(), committed_new);
}

TEST_F(TMVarTest, StealAbortedLockViaNodeGeneration) {
    TMVar<int> var(10);
    TxDescriptor tx_dead(100);
    TxDescriptor tx_alive(200);
    int val1 = 20;
    int val2 = 30;
    TxDescriptor* conflict = nullptr;

    auto* first = var.tryWriteAndGetRecord(&tx_dead, &val1, conflict);
    ASSERT_NE(first, nullptr);
    tx_dead.status.store(TxStatus::ABORTED, std::memory_order_release);

    auto* second = var.tryWriteAndGetRecord(&tx_alive, &val2, conflict);
    ASSERT_NE(second, nullptr);
    ASSERT_EQ(var.readProxy(&tx_alive), 30);

    tx_alive.status.store(TxStatus::COMMITTED, std::memory_order_release);
    ASSERT_TRUE(var.commitReleaseRecord(210));
    ASSERT_EQ(var.getDataVersion(), 210);
}

// Regression: a reader may retain a published WriteRecord after a helper
// removes it from head.  The record must remain alive until the reader leaves.
TEST_F(TMVarTest, PublishedWriteRecordRetiresAfterHelper) {
#if STM_WW_VERIFY_LOGIC_MODE
    GTEST_SKIP() << "This interleaving exercises the mode=0 EBR path.";
#else
    using TestVar = TMVar<LifetimeValue>;
    using TestRecord = typename TestVar::RecordT;

    EBRManager* mgr = EBRManager::instance();
    TestRecord::debug_destructor_count.store(0, std::memory_order_relaxed);

    TestVar var(LifetimeValue{0});
    TxDescriptor writer(100);
    PublishedRecordState state;
    std::atomic<TestRecord*> held_record{nullptr};

    std::thread reader_thread([&] {
        state.published.wait_until(1);
        mgr->enter();

        auto* record = var.debugLoadRecordForTest();
        held_record.store(record, std::memory_order_release);
        state.reader_loaded.signal();
        state.allow_reader_dereference.wait_until(1);

        const bool live = record != nullptr
            && record->owner == &writer
            && record->new_node != nullptr
            && record->new_node->payload.value == 7;
        state.reader_saw_live_record.store(live, std::memory_order_release);
        state.reader_dereferenced.signal();
        mgr->leave();
    });

    LifetimeValue draft{7};
    TxDescriptor* conflict = nullptr;
    void* result = var.tryWriteAndGetRecord(&writer, &draft, conflict);
    ASSERT_NE(result, nullptr);
    ASSERT_EQ(conflict, nullptr);
    state.published.signal();
    state.reader_loaded.wait_until(1);

    writer.status.store(TxStatus::ABORTED, std::memory_order_release);
    ASSERT_TRUE(var.debugHelpCurrentRecordForTest());
    ASSERT_NE(held_record.load(std::memory_order_acquire), nullptr);
    ASSERT_EQ(TestRecord::debug_destructor_count.load(std::memory_order_relaxed), 0);
    mgr->leave();

    state.allow_reader_dereference.signal();
    state.reader_dereferenced.wait_until(1);
    reader_thread.join();
    ASSERT_TRUE(state.reader_saw_live_record.load(std::memory_order_acquire));

    for (int i = 0;
         i < 4 && TestRecord::debug_destructor_count.load(std::memory_order_relaxed) == 0;
         ++i) {
        mgr->enter();
        mgr->leave();
    }
    ASSERT_EQ(TestRecord::debug_destructor_count.load(std::memory_order_relaxed), 1);
#endif
}

TEST_F(TMVarTest, PublishedWriteRecordRetiredExactlyOnce) {
#if STM_WW_VERIFY_LOGIC_MODE
    GTEST_SKIP() << "This invariant exercises the mode=0 EBR path.";
#else
    using TestVar = TMVar<LifetimeValue>;
    using TestRecord = typename TestVar::RecordT;

    EBRManager* mgr = EBRManager::instance();
    TestRecord::debug_destructor_count.store(0, std::memory_order_relaxed);

    TestVar var(LifetimeValue{0});
    TxDescriptor writer(300);
    LifetimeValue draft{11};
    TxDescriptor* conflict = nullptr;
    void* record = var.tryWriteAndGetRecord(&writer, &draft, conflict);
    ASSERT_NE(record, nullptr);

    writer.status.store(TxStatus::ABORTED, std::memory_order_release);
    var.abortRestoreData(record);
    // The saved pointer is only an ownership token now.  A second cleanup
    // must observe head != record and must not retire it again.
    var.abortRestoreData(record);

    ASSERT_EQ(TestRecord::debug_destructor_count.load(std::memory_order_relaxed), 0);
    mgr->leave();
    for (int i = 0;
         i < 4 && TestRecord::debug_destructor_count.load(std::memory_order_relaxed) == 0;
         ++i) {
        mgr->enter();
        mgr->leave();
    }
    ASSERT_EQ(TestRecord::debug_destructor_count.load(std::memory_order_relaxed), 1);
#endif
}

TEST_F(TMVarTest, OwnerCleanupAfterHelperUsesTokenOnly) {
#if STM_WW_VERIFY_LOGIC_MODE
    GTEST_SKIP() << "This invariant exercises the mode=0 EBR path.";
#else
    using TestVar = TMVar<LifetimeValue>;
    using TestRecord = typename TestVar::RecordT;

    EBRManager* mgr = EBRManager::instance();
    TestRecord::debug_destructor_count.store(0, std::memory_order_relaxed);

    TestVar var(LifetimeValue{0});
    TxDescriptor writer(400);
    LifetimeValue draft{13};
    TxDescriptor* conflict = nullptr;
    void* record = var.tryWriteAndGetRecord(&writer, &draft, conflict);
    ASSERT_NE(record, nullptr);

    writer.status.store(TxStatus::ABORTED, std::memory_order_release);
    ASSERT_TRUE(var.debugHelpCurrentRecordForTest());

    // The owner may still retain this raw write-set token when a helper has
    // already flattened and retired the locator.  Cleanup must compare the
    // head token and return without reading the retired record fields.
    var.abortRestoreData(record);
    ASSERT_EQ(TestRecord::debug_destructor_count.load(std::memory_order_relaxed), 0);

    mgr->leave();
    for (int i = 0;
         i < 4 && TestRecord::debug_destructor_count.load(std::memory_order_relaxed) == 0;
         ++i) {
        mgr->enter();
        mgr->leave();
    }
    ASSERT_EQ(TestRecord::debug_destructor_count.load(std::memory_order_relaxed), 1);
#endif
}
