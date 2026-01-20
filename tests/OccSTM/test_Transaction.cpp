#include <gtest/gtest.h>
#include "OccSTM/Transaction.hpp"

// 假设这些组件已经正确实现并链接
// 如果 StripedLockTable 的实现包含在 .cpp 中，请确保链接

using namespace STM::Occ;

class TransactionTest : public ::testing::Test {
protected:
    void SetUp() override {
        // 每个测试开始前重置描述符
        desc.reset();
    }

    void TearDown() override {
        // 清理可能残留的锁（防止测试间干扰）
        // 注意：在真实场景中 Transaction 析构或 reset 会处理
        desc.reset(); 
    }

    // 每个测试独立的描述符
    TransactionDescriptor desc;
};

// ============================================================================
// 1. 基础功能测试
// ============================================================================

// 测试：开启事务，写入值，提交，验证值确实被修改
TEST_F(TransactionTest, BasicStoreAndCommit) {
    TMVar<int> var(10);
    Transaction tx(&desc);

    tx.begin();
    tx.store(var, 20);
    
    EXPECT_TRUE(tx.commit());

    // 验证提交后的最新值
    auto* head = var.loadHead();
    EXPECT_EQ(head->payload, 20);
    EXPECT_GT(head->write_ts, 0); // 时间戳应该被更新
}

// 测试：Read-Your-Own-Writes (在同一个事务中读到自己刚写的值)
TEST_F(TransactionTest, ReadYourOwnWrites) {
    TMVar<int> var(10);
    Transaction tx(&desc);

    tx.begin();
    int val1 = tx.load(var);
    EXPECT_EQ(val1, 10); // 初始值

    tx.store(var, 20);
    int val2 = tx.load(var);
    EXPECT_EQ(val2, 20); // 应该读到写集中的新值

    tx.store(var, 30);
    int val3 = tx.load(var);
    EXPECT_EQ(val3, 30); // 更新后的写集值

    EXPECT_TRUE(tx.commit());
    EXPECT_EQ(var.loadHead()->payload, 30);
}

// ============================================================================
// 2. Occ 与隔离性测试
// ============================================================================

// 测试：快照隔离 (只读事务应该读到事务开始时的旧值，即使由于并发有了新提交)
TEST_F(TransactionTest, SnapshotIsolation) {
    TMVar<int> var(100);
    
    Transaction tx_reader(&desc);
    TransactionDescriptor desc_writer;
    Transaction tx_writer(&desc_writer);

    // 1. Reader 开始事务 (RV = T1)
    tx_reader.begin();
    // 此时 Reader 还没读，但在逻辑上它处于 T1

    // 2. Writer 并发介入，修改并提交 (WV = T2 > T1)
    tx_writer.begin();
    tx_writer.store(var, 200);
    EXPECT_TRUE(tx_writer.commit()); // var 变成了 200

    // 3. Reader 现在才来读
    // 根据 Occ，它应该看不到 200，只能看到 100
    int val = tx_reader.load(var);
    EXPECT_EQ(val, 100);

    // 4. Reader 提交 (只读优化路径)
    EXPECT_TRUE(tx_reader.commit());
}

// 测试：只读优化 (Read-Only Optimization)
// 即使读的数据后来被修改了，只要 load 成功，只读事务就应该能提交
TEST_F(TransactionTest, ReadOnlyOptimization) {
    TMVar<int> var(10);
    Transaction tx(&desc);

    tx.begin();
    tx.load(var);
    
    // 模拟外部修改：手动推进 GlobalClock 并修改 var 的时间戳
    // 让 var 看起来像是被别人改过了
    uint64_t new_ts = GlobalClock::tick();
    var.loadHead()->write_ts = new_ts; 

    // 正常情况下 Validate 会失败 (head.ts > rv)。
    // 但因为 writeSet 为空，代码应该走优化路径直接返回 true。
    EXPECT_TRUE(tx.commit());
}

// ============================================================================
// 3. 冲突与验证测试 (Serializable)
// ============================================================================

// 测试：读写事务的 Validate 失败 (因为读过的数据被别人修改了)
TEST_F(TransactionTest, ValidationFail_Timestamp) {
    TMVar<int> x(10);
    TMVar<int> y(20);
    
    Transaction tx(&desc);
    TransactionDescriptor desc_other;
    Transaction tx_other(&desc_other);

    // 1. Tx 开始，读 X
    tx.begin();
    int val_x = tx.load(x);
    
    // 2. 外部干扰：Tx_other 修改 X 并提交
    tx_other.begin();
    tx_other.store(x, 11);
    EXPECT_TRUE(tx_other.commit()); // X 的 ts 变大了

    // 3. Tx 尝试写 Y (使其成为读写事务，强制 Validate)
    tx.store(y, 21);

    // 4. Tx 提交 -> 应该失败！
    // 因为 X 的 ts > Tx.rv，违背了串行化
    EXPECT_FALSE(tx.commit());
}

// 测试：Validate 失败 (因为读过的数据正被别人锁住)
TEST_F(TransactionTest, ValidationFail_LockedByOther) {
    TMVar<int> x(10);
    TMVar<int> y(20);
    
    Transaction tx(&desc);

    tx.begin();
    tx.load(x); // 加入 ReadSet

    // 模拟干扰：手动锁住 X
    StripedLockTable::instance().lock(&x);

    // Tx 尝试写 Y (强制 Validate)
    tx.store(y, 21);

    // 提交 -> 应该检测到锁冲突返回 false
    // 注意：这里我们是在同一个线程模拟，但逻辑上 lock_table 不管线程ID(除非是递归锁)，
    // 而是由 Transaction::validateReadSet 中的逻辑判断 "locked_by_me"。
    // 由于 X 不在 tx 的 writeSet 中，locked_by_me 为 false。
    bool result = tx.commit();
    
    StripedLockTable::instance().unlock(&x); // 清理现场

    EXPECT_FALSE(result);
}

// 测试：Validate 成功 (即使锁住了，但是是我自己锁的)
TEST_F(TransactionTest, ValidationSuccess_LockedByMe) {
    TMVar<int> x(10);
    
    Transaction tx(&desc);

    tx.begin();
    // 既读又写
    tx.load(x); 
    tx.store(x, 20);

    // 在 commit 过程中：
    // 1. lockWriteSet() 会锁住 x
    // 2. validateReadSet() 会检查 x 是否被锁
    // 3. 此时 x 被锁，但在 writeSet 中，应判为 locked_by_me = true，通过验证
    EXPECT_TRUE(tx.commit());
}

// ============================================================================
// 4. 异常处理测试
// ============================================================================

// 测试：RetryException (历史版本被剪枝)
TEST_F(TransactionTest, PrunedHistoryRetry) {
    TMVar<int> var(0);
    TransactionDescriptor desc_updater;
    Transaction tx_updater(&desc_updater);

    // 1. 制造大量历史版本，触发剪枝
    // MAX_HISTORY = 8。我们提交 10 次。
    for(int i=1; i<=10; ++i) {
        tx_updater.begin();
        tx_updater.store(var, i);
        EXPECT_TRUE(tx_updater.commit());
    }

    // 2. 构造一个非常老的事务
    Transaction tx_old(&desc);
    tx_old.begin();
    
    // 强行把读版本号设为 0 (远古时期)
    desc.setReadVersion(0); 

    // 3. 尝试读取
    // 现在的链表应该是 10 -> 9 -> ... -> 3 (大致)，0 已经被物理删除了
    // 遍历到底部发现所有版本都 > 0，抛出 RetryException
    EXPECT_THROW({
        tx_old.load(var);
    }, RetryException);
}