#include <gtest/gtest.h>
#include <thread>
#include <chrono>

// 包含你的核心头文件
#include "WwSTM/TxContext.hpp"
#include "WwSTM/TMVar.hpp"

using namespace STM::Ww;

// 测试夹具 (Fixture)，用于每个测试前的环境重置（如果需要）
class OSTMTest : public ::testing::Test {
protected:
    void SetUp() override {
        // 如果 GlobalClock 或 EBR 需要重置，可以在这里做
        // 目前看来不需要
    }
};

// =========================================================
// 1. 基础功能测试 (Happy Path)
// =========================================================

// 测试：空事务提交
TEST_F(OSTMTest, EmptyTransaction) {
    TxContext tx;
    // 刚开始应该是有效的
    ASSERT_TRUE(tx.commit()); 
}

// 测试：单个变量的读写与 In-Place Update
TEST_F(OSTMTest, SingleVarReadWrite) {
    TMVar<int> var(10); // 初始值 10

    TxContext tx;
    
    // 1. 读初始值
    ASSERT_EQ(tx.read(var), 10);

    // 2. 写新值
    tx.write(var, 20);

    // 3. Read-Your-Own-Writes (必须读到脏数据)
    ASSERT_EQ(tx.read(var), 20);

    // 4. 提交
    ASSERT_TRUE(tx.commit());
}

// 测试：事务提交后的持久性 (Isolation)
TEST_F(OSTMTest, CommitPersistence) {
    TMVar<int> var(100);

    // T1 修改
    {
        TxContext tx1;
        tx1.write(var, 200);
        ASSERT_TRUE(tx1.commit());
    }

    // T2 读取
    {
        TxContext tx2;
        ASSERT_EQ(tx2.read(var), 200); // 应该读到 200
        tx2.commit();
    }
}

// =========================================================
// 2. 异常与回滚测试 (Abort Logic)
// =========================================================

// 测试：析构触发自动 Abort
TEST_F(OSTMTest, DestructorAborts) {
    TMVar<int> var(500);

    {
        TxContext tx;
        tx.write(var, 600);
        ASSERT_EQ(tx.read(var), 600);
        // tx 在这里析构，没有调用 commit，应该触发 abortImpl
    }

    // 验证回滚
    TxContext tx2;
    ASSERT_EQ(tx2.read(var), 500); // 必须回滚到 500
    tx2.commit();
}

// 测试：多变量 Abort 一致性
TEST_F(OSTMTest, MultiVarAbort) {
    TMVar<int> v1(1);
    TMVar<int> v2(2);

    {
        TxContext tx;
        tx.write(v1, 10);
        tx.write(v2, 20);
        // 模拟中途失败
    }

    TxContext tx_check;
    ASSERT_EQ(tx_check.read(v1), 1);
    ASSERT_EQ(tx_check.read(v2), 2);
    tx_check.commit();
}

// =========================================================
// 3. 冲突逻辑测试 (Wound-Wait Logic)
// =========================================================

// 辅助：模拟两个事务，验证老杀少
TEST_F(OSTMTest, WoundWait_OldKillsYoung) {
    TMVar<int> var(10);

    // 1. 创建 Tx_Young (先创建，但我们假设 start_ts 随时间增加)
    // 注意：TxContext 构造时就获取时间戳。
    TxContext* tx_young = new TxContext(); 
    
    // 强制 sleep 确保时间戳推进
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    
    // 创建 Tx_Old ??? 等等，Wound-Wait 是 StartTS 越小越老。
    // 所以先创建的是 "Old"，后创建的是 "Young"。
    
    // 修正测试逻辑：
    // Tx1 (Old) 先开始
    TxContext* tx_old = tx_young; // 复用指针名，逻辑上它是 TS 较小的
    
    // Tx2 (Young) 后开始
    TxContext* tx_new = new TxContext(); 

    // 2. 年轻人 (Tx_New) 先抢占锁
    tx_new->write(var, 20); // var 现在被 tx_new 锁定

    // 3. 老人 (Tx_Old) 尝试写 -> 应该触发 Wound (杀掉 Tx_New)
    // 预期：tx_old 成功抢占（Steal），或者等待 tx_new 变成 ABORTED
    tx_old->write(var, 30); 

    // 4. 验证老人写入成功
    ASSERT_EQ(tx_old->read(var), 30);
    ASSERT_TRUE(tx_old->commit()); // 老人提交成功

    // 5. 验证年轻人已死
    // 年轻人尝试提交应该失败 (commit 内部 checkValidity 会发现被改为 ABORTED)
    ASSERT_FALSE(tx_new->commit()); 

    delete tx_old; // 实际上是 delete 第一次 new 的
    delete tx_new;

    // 6. 全局验证
    TxContext tx_final;
    ASSERT_EQ(tx_final.read(var), 30); // 应该是老人写的值
    tx_final.commit();
}

// 辅助：模拟少让老 (Young Waits/Dies)
TEST_F(OSTMTest, WoundWait_YoungDies) {
    TMVar<int> var(10);

    // Tx_Old (TS 小)
    TxContext* tx_old = new TxContext();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    // Tx_Young (TS 大)
    TxContext* tx_young = new TxContext();

    // 1. 老人先持有锁
    tx_old->write(var, 88);

    // 2. 年轻人尝试写 -> 发现锁属于老人 -> 应该自杀 (Abort)
    // 这一步在你的代码逻辑里，会在 write 内部调用 abortImpl 并 return
    tx_young->write(var, 99);

    // 3. 验证年轻人自杀成功
    // 此时 tx_young 内部 is_valid_ 应该为 false
    // 任何操作都应该无效或返回 false
    ASSERT_FALSE(tx_young->commit());

    // 4. 老人继续提交
    ASSERT_TRUE(tx_old->commit());

    delete tx_old;
    delete tx_young;

    // 5. 验证值
    TxContext tx_final;
    ASSERT_EQ(tx_final.read(var), 88); 
    tx_final.commit();
}

// main 函数入口
int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}