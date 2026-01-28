#include <gtest/gtest.h>
#include <thread>
#include <atomic>
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

