#include <gtest/gtest.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "WwSTM/TMVar.hpp"
#include "WwSTM/TxContext.hpp"
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

struct CommitPauseState {
    PhaseGate entered;
    PhaseGate release;
    TxDescriptor* descriptor = nullptr;
};

CommitPauseState* active_prepare_pause_state = nullptr;
CommitPauseState* active_committed_pause_state = nullptr;

template<typename T>
bool commitDirect(TMVar<T>& var,
                  void* record,
                  TxDescriptor& tx,
                  uint64_t commit_ts) {
    if (!var.prepareCommit(record, &tx, commit_ts)) return false;
    if (!TxStatusHelper::tryCommit(tx.status)) return false;
    var.helpComplete(record);
    return true;
}

void changeHeadDuringRead(TMVar<int>& var) {
    auto* state = active_head_change_state;
    if (!state || !state->first_hook.exchange(false, std::memory_order_acq_rel)) {
        return;
    }

    state->writer->status.store(TxStatus::ABORTED, std::memory_order_release);
    var.debugHelpCurrentRecordForTest();
}

void pauseAfterPrepare(TxContext& tx) {
    auto* state = active_prepare_pause_state;
    if (!state) return;
    state->descriptor = tx.debugDescriptorForTest();
    state->entered.signal();
    state->release.wait_until(1);
}

void pauseAfterCommitted(TxContext&) {
    auto* state = active_committed_pause_state;
    if (!state) return;
    state->entered.signal();
    state->release.wait_until(1);
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

    ASSERT_TRUE(commitDirect(var, rec, tx, 105));

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

    void* record = var.tryWriteAndGetRecord(&tx_writer, &val, conflict);
    ASSERT_NE(record, nullptr);
    ASSERT_EQ(var.readProxy(&tx_reader), 100);

    ASSERT_TRUE(commitDirect(var, record, tx_writer, 15));
    ASSERT_EQ(var.readProxy(&tx_reader), 200);
}

TEST_F(TMVarTest, WriteWriteConflict) {
    TMVar<int> var(10);
    TxDescriptor tx1(100);
    TxDescriptor tx2(200);
    int val1 = 20;
    int val2 = 30;
    TxDescriptor* conflict = nullptr;

    void* record = var.tryWriteAndGetRecord(&tx1, &val1, conflict);
    ASSERT_NE(record, nullptr);
    conflict = nullptr;
    ASSERT_EQ(var.tryWriteAndGetRecord(&tx2, &val2, conflict), nullptr);
    ASSERT_EQ(conflict, &tx1);

    ASSERT_TRUE(commitDirect(var, record, tx1, 110));
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

    ASSERT_TRUE(commitDirect(var, first, tx, 60));

    TxDescriptor tx_check(70);
    ASSERT_EQ(var.readProxy(&tx_check), 3);
}

TEST_F(TMVarTest, PrepareFreezesDraftAndPublishesFinalTimestamp) {
    TMVar<int> var(0);
    TxDescriptor tx(50);
    TxDescriptor* conflict = nullptr;
    int first_value = 2;
    int late_value = 3;

    void* record = var.tryWriteAndGetRecord(&tx, &first_value, conflict);
    ASSERT_NE(record, nullptr);
    auto* typed_record = static_cast<typename TMVar<int>::RecordT*>(record);

    ASSERT_TRUE(var.prepareCommit(record, &tx, 75));
    ASSERT_TRUE(typed_record->prepared.load(std::memory_order_acquire));
    ASSERT_EQ(typed_record->new_node->loadWriteTs(), 75);

    conflict = nullptr;
    ASSERT_EQ(var.tryWriteAndGetRecord(&tx, &late_value, conflict), nullptr);
    ASSERT_EQ(conflict, nullptr);
    ASSERT_EQ(typed_record->new_node->payload, 2);

    ASSERT_TRUE(TxStatusHelper::tryCommit(tx.status));
    var.helpComplete(record);
    TxDescriptor reader(80);
    ASSERT_EQ(var.readProxy(&reader), 2);
    ASSERT_EQ(var.getDataVersion(), 75);
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
    void* record2 = var.tryWriteAndGetRecord(&tx2, &val2, conflict);
    ASSERT_NE(record2, nullptr);
    ASSERT_TRUE(commitDirect(var, record2, tx2, 210));
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
    ASSERT_TRUE(var.prepareCommit(raw_record, &writer, 15));
    ASSERT_TRUE(TxStatusHelper::tryCommit(writer.status));

    ASSERT_EQ(var.debugLoadRecordForTest(), record);
    auto snapshot = var.readSnapshot(&reader);
    ASSERT_EQ(snapshot.value, 20);
    ASSERT_EQ(snapshot.version, 15);
    ASSERT_EQ(snapshot.node, record->new_node);
    ASSERT_EQ(var.debugLoadRecordForTest(), record);

    ASSERT_TRUE(var.debugHelpCurrentRecordForTest());
    ASSERT_EQ(var.debugLoadNodeForTest(), record->new_node);
}

TEST_F(TMVarTest, MultiVariableCommitLinearizesAtDescriptorStatus) {
    TMVar<int> x(10);
    TMVar<int> y(20);
    CommitPauseState state;
    std::atomic<bool> committed{false};

    active_prepare_pause_state = &state;
    TxContext::setPrepareHook(&pauseAfterPrepare);
    std::thread writer([&] {
        TxContext tx;
        tx.write(&x, 11);
        tx.write(&y, 21);
        committed.store(tx.commit(), std::memory_order_release);
    });

    state.entered.wait_until(1);
    ASSERT_NE(state.descriptor, nullptr);
    ASSERT_EQ(state.descriptor->status.load(std::memory_order_acquire), TxStatus::ACTIVE);

    TxContext reader;
    ASSERT_EQ(reader.read(&x), 10);
    ASSERT_EQ(reader.read(&y), 20);
    ASSERT_TRUE(reader.commit());

    state.release.signal();
    writer.join();
    TxContext::clearPrepareHook();
    active_prepare_pause_state = nullptr;

    ASSERT_TRUE(committed.load(std::memory_order_acquire));

    TxContext committed_reader;
    ASSERT_EQ(committed_reader.read(&x), 11);
    ASSERT_EQ(committed_reader.read(&y), 21);
    ASSERT_TRUE(committed_reader.commit());
}

TEST_F(TMVarTest, AbortMultiVariableKeepsOldLogicalView) {
    TMVar<int> x(10);
    TMVar<int> y(20);
    CommitPauseState state;
    std::atomic<bool> committed{true};

    active_prepare_pause_state = &state;
    TxContext::setPrepareHook(&pauseAfterPrepare);
    std::thread writer([&] {
        TxContext tx;
        tx.write(&x, 11);
        tx.write(&y, 21);
        committed.store(tx.commit(), std::memory_order_release);
    });

    state.entered.wait_until(1);
    ASSERT_NE(state.descriptor, nullptr);

    TxContext reader;
    ASSERT_EQ(reader.read(&x), 10);
    ASSERT_EQ(reader.read(&y), 20);

    state.descriptor->status.store(TxStatus::ABORTED, std::memory_order_release);
    state.release.signal();
    writer.join();
    TxContext::clearPrepareHook();
    active_prepare_pause_state = nullptr;

    ASSERT_FALSE(committed.load(std::memory_order_acquire));
    ASSERT_TRUE(reader.commit());

    TxContext verifier;
    ASSERT_EQ(verifier.read(&x), 10);
    ASSERT_EQ(verifier.read(&y), 20);
    ASSERT_TRUE(verifier.commit());
}

TEST_F(TMVarTest, MultiVariableAtomicityStress) {
    TMVar<int> x(0);
    TMVar<int> y(0);
    std::atomic<int> next_value{1};
    std::atomic<int> mixed_snapshots{0};
    std::atomic<int> successful_reads{0};

    auto writer = [&] {
        TxContext tx;
        for (int i = 0; i < 100; ++i) {
            bool committed = false;
            while (!committed) {
                tx.begin();
                const int value = next_value.fetch_add(1, std::memory_order_relaxed);
                tx.write(&x, value);
                if (!tx.isActive()) continue;
                tx.write(&y, value);
                if (!tx.isActive()) continue;
                committed = tx.commit();
            }
        }
    };

    auto reader = [&] {
        TxContext tx;
        for (int i = 0; i < 200; ++i) {
            bool committed = false;
            while (!committed) {
                tx.begin();
                const int x_value = tx.read(&x);
                if (!tx.isActive()) continue;
                const int y_value = tx.read(&y);
                if (!tx.isActive()) continue;

                committed = tx.commit();
                if (committed) {
                    successful_reads.fetch_add(1, std::memory_order_relaxed);
                    if (x_value != y_value) {
                        mixed_snapshots.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
        }
    };

    std::vector<std::thread> writers;
    std::vector<std::thread> readers;
    writers.reserve(4);
    readers.reserve(2);
    for (int i = 0; i < 4; ++i) writers.emplace_back(writer);
    for (int i = 0; i < 2; ++i) readers.emplace_back(reader);
    for (auto& thread : writers) thread.join();
    for (auto& thread : readers) thread.join();

    ASSERT_EQ(mixed_snapshots.load(std::memory_order_relaxed), 0);
    ASSERT_GT(successful_reads.load(std::memory_order_relaxed), 0);

    TxContext verifier;
    const int final_x = verifier.read(&x);
    const int final_y = verifier.read(&y);
    ASSERT_TRUE(verifier.commit());
    ASSERT_EQ(final_x, final_y);
}

TEST_F(TMVarTest, PostCommitHelperRaceKeepsCommitSuccessful) {
    TMVar<int> var(0);
    CommitPauseState state;
    std::atomic<bool> committed{false};

    active_committed_pause_state = &state;
    TxContext::setCommittedHook(&pauseAfterCommitted);
    std::thread writer([&] {
        TxContext tx;
        tx.write(&var, 1);
        committed.store(tx.commit(), std::memory_order_release);
    });

    state.entered.wait_until(1);
    ASSERT_TRUE(var.debugHelpCurrentRecordForTest());

    state.release.signal();
    writer.join();
    TxContext::clearCommittedHook();
    active_committed_pause_state = nullptr;

    ASSERT_TRUE(committed.load(std::memory_order_acquire));
    TxContext reader;
    ASSERT_EQ(reader.read(&var), 1);
    ASSERT_TRUE(reader.commit());
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
    ASSERT_TRUE(committed_var.prepareCommit(committed_raw, &committed_writer, 25));
    ASSERT_TRUE(TxStatusHelper::tryCommit(committed_writer.status));
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

    ASSERT_TRUE(commitDirect(var, second, tx_alive, 210));
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
