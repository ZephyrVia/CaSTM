#include <gtest/gtest.h>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <vector>

// ==========================================
// 1. 直接引用你的头文件
// ==========================================
// 假设你的 include路径配置正确，或者根据实际目录结构修改引用路径
#include "WwSTM/TMVar.hpp"
#include "WwSTM/TxDescriptor.hpp"
#include "WwSTM/TxStatus.hpp"
#include "EBRManager/EBRManager.hpp" 

// 如果你的 ThreadHeap 需要显式初始化，请引入
// #include "TierAlloc/ThreadHeap/ThreadHeap.hpp"

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

struct PublishedRollbackState {
    TxDescriptor* writer = nullptr;
    PhaseGate published;
    PhaseGate reader_loaded;
    PhaseGate allow_reader_dereference;
    PhaseGate reader_dereferenced;
    std::atomic<bool> reader_saw_live_record{false};
    std::atomic<bool> first_hook{true};
};

PublishedRollbackState* active_published_rollback_state = nullptr;

void forcePublishedRollback(TMVar<LifetimeValue>& var) {
    auto* state = active_published_rollback_state;
    if (!state || !state->first_hook.exchange(false, std::memory_order_acq_rel)) {
        return;
    }

    // The install CAS has completed. Wait until a reader has loaded the raw
    // record pointer and entered EBR before changing the stable pointer.
    state->published.signal();
    state->reader_loaded.wait_until(1);

    // This deliberately makes the writer's stability recheck fail while its
    // record is still published. The writer is then aborted so the next loop
    // cannot publish a second candidate.
    var.debugReplaceStableForTest(1, LifetimeValue{1});
    state->writer->status.store(TxStatus::ABORTED, std::memory_order_release);
}

} // namespace

// ==========================================
// 2. 测试夹具 (Fixture)
// ==========================================
class TMVarTest : public ::testing::Test {
protected:
    void SetUp() override {
        // 如果 EBRManager 或 ThreadHeap 需要全局初始化，在这里调用
        // 例如: STM::EBRManager::instance()->init();
    }

    void TearDown() override {
        // 清理工作
    }
};

// ==========================================
// 3. 测试用例
// ==========================================

// 测试: 构造与默认值
TEST_F(TMVarTest, Construction) {
    // 创建一个初始值为 100 的变量
    TMVar<int> var(100);
    
    // 创建一个测试用的事务描述符 (TS=1)
    TxDescriptor tx(1);

    // 初始读取应该读到 100
    ASSERT_EQ(var.readProxy(&tx), 100);
    
    // 初始版本号应该是 0 (构造函数里设定的)
    ASSERT_EQ(var.getDataVersion(), 0);
}

// 测试: 读自己写的数据 (Read Your Own Writes)
TEST_F(TMVarTest, ReadYourOwnWrites) {
    TMVar<int> var(10);
    TxDescriptor tx(100); // StartTS = 100

    // 1. 尝试写入 20
    int write_val = 20;
    TxDescriptor* conflict = nullptr;
    auto* rec = var.tryWriteAndGetRecord(&tx, &write_val, conflict);

    ASSERT_NE(rec, nullptr) << "应该成功获取锁";
    ASSERT_EQ(conflict, nullptr) << "不应该有冲突";

    // 2. 在提交前读取，应该读到自己的修改 (20)
    int read_val = var.readProxy(&tx);
    ASSERT_EQ(read_val, 20);

    // 3. 提交
    tx.status.store(TxStatus::COMMITTED, std::memory_order_release);
    var.commitReleaseRecord(105); // CommitTS = 105

    // 4. 再次读取，验证数据已持久化
    TxDescriptor tx2(200);
    ASSERT_EQ(var.readProxy(&tx2), 20);
    ASSERT_EQ(var.getDataVersion(), 105);
}

// 测试: 隔离性 (Snapshot Read)
// 场景: Tx1 写了但没提交，Tx2 应该读到旧值
TEST_F(TMVarTest, Isolation_SnapshotRead) {
    TMVar<int> var(100); // 初始值 100
    TxDescriptor tx_writer(10); 
    TxDescriptor tx_reader(20); 

    // Writer 写入 200，状态仍为 ACTIVE
    int val = 200;
    TxDescriptor* conflict = nullptr;
    var.tryWriteAndGetRecord(&tx_writer, &val, conflict);

    // Reader 读取
    // 因为 Writer 还没提交，Reader 应该读到 OldNode (100)
    int read_val = var.readProxy(&tx_reader);
    ASSERT_EQ(read_val, 100) << "应该读到快照数据，而不是未提交的数据";

    // Writer 提交
    tx_writer.status.store(TxStatus::COMMITTED, std::memory_order_release);
    var.commitReleaseRecord(15);

    // Reader 再次读取，现在应该看到 200
    ASSERT_EQ(var.readProxy(&tx_reader), 200);
}

// 测试: 写写冲突 (Wound-Wait 逻辑)
TEST_F(TMVarTest, WriteWriteConflict) {
    TMVar<int> var(10);
    TxDescriptor tx1(100); // 先来的
    TxDescriptor tx2(200); // 后来的

    // tx1 拿锁
    int val1 = 20;
    TxDescriptor* c = nullptr;
    var.tryWriteAndGetRecord(&tx1, &val1, c);

    // tx2 尝试写
    // 根据你的逻辑: if(status == ACTIVE) return conflict;
    int val2 = 30;
    TxDescriptor* conflict_out = nullptr;
    auto* rec2 = var.tryWriteAndGetRecord(&tx2, &val2, conflict_out);

    ASSERT_EQ(rec2, nullptr) << "Tx2 应该写入失败";
    ASSERT_EQ(conflict_out, &tx1) << "冲突对象应该是 Tx1";
    
    // 清理: 让 Tx1 提交，防止内存泄漏检测报错
    tx1.status.store(TxStatus::COMMITTED);
    var.commitReleaseRecord(110);
}

// 测试: 重入写入 (Re-entrant Write)
// 同一个事务多次写入同一个变量
TEST_F(TMVarTest, ReentrantWrite) {
    TMVar<int> var(0);
    TxDescriptor tx(50);

    TxDescriptor* c = nullptr;
    
    // 第一次写 1
    int v1 = 1;
    var.tryWriteAndGetRecord(&tx, &v1, c);
    ASSERT_EQ(var.readProxy(&tx), 1);

    // 第二次写 2
    int v2 = 2;
    auto* rec = var.tryWriteAndGetRecord(&tx, &v2, c);
    ASSERT_NE(rec, nullptr);

    // 验证值更新
    ASSERT_EQ(var.readProxy(&tx), 2);

    // 提交
    tx.status.store(TxStatus::COMMITTED);
    var.commitReleaseRecord(60);
    
    // 验证最终结果
    TxDescriptor tx_check(70);
    ASSERT_EQ(var.readProxy(&tx_check), 2);
}

// 测试: 事务回滚 (Abort)
TEST_F(TMVarTest, AbortAndRollback) {
    TMVar<int> var(50);
    TxDescriptor tx(100);

    int val = 99;
    TxDescriptor* c = nullptr;
    // 保存 record 指针用于回滚
    void* rec_ptr = var.tryWriteAndGetRecord(&tx, &val, c);

    // 此时读应该是 99
    ASSERT_EQ(var.readProxy(&tx), 99);

    // 模拟回滚
    tx.status.store(TxStatus::ABORTED, std::memory_order_release);
    var.abortRestoreData(rec_ptr);

    // 验证数据回滚到 50
    TxDescriptor tx2(200);
    ASSERT_EQ(var.readProxy(&tx2), 50);

    // 验证锁已被释放，新事务可以写入
    int val2 = 60;
    auto* new_rec = var.tryWriteAndGetRecord(&tx2, &val2, c);
    ASSERT_NE(new_rec, nullptr);
    
    // 清理
    tx2.status.store(TxStatus::COMMITTED);
    var.commitReleaseRecord(210);
}

// 测试: 锁窃取 (Steal Lock from ABORTED Tx)
// 测试 Wound-Wait 策略中的 "Steal" 分支
TEST_F(TMVarTest, StealAbortedLock) {
    TMVar<int> var(10);
    TxDescriptor tx_dead(100);
    TxDescriptor tx_alive(200);

    // tx_dead 拿到锁
    int val1 = 20;
    TxDescriptor* c = nullptr;
    var.tryWriteAndGetRecord(&tx_dead, &val1, c);

    // tx_dead 变为 ABORTED (比如被 Wound 了，或者崩溃了)，但没有主动调 abortRestoreData
    tx_dead.status.store(TxStatus::ABORTED, std::memory_order_release);

    // tx_alive 尝试写
    // 代码逻辑应该是: 检测到 owner 是 ABORTED -> Steal -> CAS
    int val2 = 30;
    auto* rec = var.tryWriteAndGetRecord(&tx_alive, &val2, c);

    ASSERT_NE(rec, nullptr) << "应该成功从 Aborted 事务手中抢锁";
    
    // 验证值是新事务写的 30
    ASSERT_EQ(var.readProxy(&tx_alive), 30);

    // 提交
    tx_alive.status.store(TxStatus::COMMITTED);
    var.commitReleaseRecord(210);
    
    ASSERT_EQ(var.getDataVersion(), 210);
}


// 测试: ABORTED 记录在位时，普通读者必须读 stable data_ptr_，而非 record->old_node
// 回归背景（2026-09 取证）：ABORTED 记录的 old_node 是安装时刻的快照，可能落后于
// data_ptr_。readProxy 曾从 old_node 取值，拼出不存在的 (version, value) 组合
// （v_pre=1943, val=1942），后续校验全部放行 → 丢失更新；old_node 被回收后
// 还会悬垂解引用。本测试用白盒手段直接构造"old_node 落后"的中间态。
TEST_F(TMVarTest, AbortedRecordReadUsesStableData) {
    TMVar<int> var(10);

    // 1. 安装一个写记录（owner 先 ACTIVE）
    TxDescriptor tx_dead(100);
    int draft_val = 999;
    TxDescriptor* c = nullptr;
    auto* rec = var.tryWriteAndGetRecord(&tx_dead, &draft_val, c);
    ASSERT_NE(rec, nullptr);

    // 2. owner 被击伤（wound），记录停留在"待偷窃"中间态
    tx_dead.status.store(TxStatus::ABORTED, std::memory_order_release);

    // 3. 白盒模拟病灶：old_node 指向一个"过期"节点（伪造落后版本）
    auto* stale_fake = new detail::VersionNode<int>(0, 777);
    static_cast<detail::WriteRecord<int>*>(rec)->old_node = stale_fake;

    // 4. 读者必须取 stable data_ptr_ 的值（10），绝不能是 old_node 的 777
    TxDescriptor tx_reader(200);
    ASSERT_EQ(var.readProxy(&tx_reader), 10) << "读值必须来自 data_ptr_，而非 old_node";

    delete stale_fake;
}

// 测试: ACTIVE 记录在位时，读者同样必须读 stable data_ptr_，而非 record->old_node
// 回归背景：曾按"ACTIVE 期间 old_node==data 恒成立"恢复 ACTIVE -> old_node，
// mode=0 计数测试随即复现 7/200 丢失更新，探针定位偏斜读来自 ACTIVE 分支
// (v_pre=1155, val=1154, old_node 版本 1154)，证实该不变式在真实交错下不成立。
TEST_F(TMVarTest, ActiveRecordReadIgnoresStaleOldNode) {
    TMVar<int> var(10);

    // 1. 安装一个写记录，owner 保持 ACTIVE（模拟持有者正在写的中间态）
    TxDescriptor tx_active(100);
    int draft_val = 999;
    TxDescriptor* c = nullptr;
    auto* rec = var.tryWriteAndGetRecord(&tx_active, &draft_val, c);
    ASSERT_NE(rec, nullptr);

    // 2. 白盒模拟病灶：old_node 指向一个"过期"节点
    auto* stale_fake = new detail::VersionNode<int>(0, 777);
    static_cast<detail::WriteRecord<int>*>(rec)->old_node = stale_fake;

    // 3. 读者必须取 stable data_ptr_ 的值（10），而不是 old_node 的 777
    TxDescriptor tx_reader(200);
    ASSERT_EQ(var.readProxy(&tx_reader), 10) << "读值必须来自 data_ptr_，而非 old_node";

    delete stale_fake;
}

// Regression: a reader may retain a published WriteRecord after the writer
// removes it from record_ptr_. The record must therefore be retired, not
// directly deleted or reused by the rollback retry.
TEST_F(TMVarTest, PublishedWriteRecordRetiresAfterStabilityRollback) {
#if STM_WW_VERIFY_LOGIC_MODE
    GTEST_SKIP() << "This interleaving exercises the mode=0 CAS path.";
#else
    using TestVar = TMVar<LifetimeValue>;
    using TestRecord = typename TestVar::RecordT;

    EBRManager* mgr = EBRManager::instance();
    TestRecord::debug_destructor_count.store(0, std::memory_order_relaxed);

    TestVar var(LifetimeValue{0});
    TxDescriptor writer(100);
    TxDescriptor reader(200);
    PublishedRollbackState state;
    state.writer = &writer;
    active_published_rollback_state = &state;
    TestVar::setStabilityCheckHook(&forcePublishedRollback);

    std::atomic<typename TestVar::RecordT*> held_record{nullptr};
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

    mgr->enter();
    LifetimeValue draft{7};
    TxDescriptor* conflict = nullptr;
    void* result = var.tryWriteAndGetRecord(&writer, &draft, conflict);
    TestVar::clearStabilityCheckHook();
    active_published_rollback_state = nullptr;

    EXPECT_EQ(result, nullptr) << "the hook aborts the writer after rollback";
    EXPECT_EQ(conflict, nullptr);
    EXPECT_EQ(held_record.load(std::memory_order_acquire) != nullptr, true);
    EXPECT_EQ(TestRecord::debug_destructor_count.load(std::memory_order_relaxed), 0)
        << "an active reader must keep the published record alive";
    mgr->leave();

    state.allow_reader_dereference.signal();
    state.reader_dereferenced.wait_until(1);
    reader_thread.join();

    EXPECT_TRUE(state.reader_saw_live_record.load(std::memory_order_acquire));

    // The reader's leave supplies one epoch transition. One further clean
    // enter/leave supplies the grace period for the record retired by T1.
    for (int i = 0; i < 4
         && TestRecord::debug_destructor_count.load(std::memory_order_relaxed) == 0;
         ++i) {
        mgr->enter();
        mgr->leave();
    }

    EXPECT_EQ(TestRecord::debug_destructor_count.load(std::memory_order_relaxed), 1)
        << "the published record must be reclaimed exactly once";
#endif
}

// Invariant: once an installed record is removed, exactly one cleanup path
// owns it. A repeated abort cleanup is a no-op and must not retire it twice.
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
    mgr->enter();

    LifetimeValue draft{11};
    TxDescriptor* conflict = nullptr;
    void* record = var.tryWriteAndGetRecord(&writer, &draft, conflict);
    EXPECT_NE(record, nullptr);
    EXPECT_EQ(conflict, nullptr);

    writer.status.store(TxStatus::ABORTED, std::memory_order_release);
    var.abortRestoreData(record);
    // The saved pointer is only an ownership token now. The second call must
    // observe record_ptr_ == nullptr and must not retire the object again.
    var.abortRestoreData(record);

    EXPECT_EQ(TestRecord::debug_destructor_count.load(std::memory_order_relaxed), 0)
        << "retirement must wait for the grace period";
    mgr->leave();

    for (int i = 0; i < 4
         && TestRecord::debug_destructor_count.load(std::memory_order_relaxed) == 0;
         ++i) {
        mgr->enter();
        mgr->leave();
    }

    EXPECT_EQ(TestRecord::debug_destructor_count.load(std::memory_order_relaxed), 1)
        << "a published record must be reclaimed exactly once";
#endif
}
