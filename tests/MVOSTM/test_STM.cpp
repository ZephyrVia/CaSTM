#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <stdexcept>
#include "MVOSTM/STM.hpp"

// 1. 基础功能测试：验证 load/store 和返回值逻辑
TEST(STMTest, BasicReadWrite) {
    STM::Var<int> account(100);

    // 测试 void 类型的 atomically
    STM::atomically([&](Transaction& tx) {
        int val = tx.load(account);
        tx.store(account, val + 50);
    });

    // 测试带返回值的 atomically
    int current_balance = STM::atomically([&](Transaction& tx) {
        return tx.load(account);
    });

    EXPECT_EQ(current_balance, 150);
}

// 2. 异常回滚测试：验证抛出异常后，修改不会生效（原子性）
TEST(STMTest, ExceptionRollback) {
    STM::Var<std::string> status("Clean");

    // 预期会抛出 runtime_error
    EXPECT_THROW({
        STM::atomically([&](Transaction& tx) {
            tx.store(status, std::string("Dirty")); // 正确：显式转换为 std::string
            throw std::runtime_error("Boom!"); // 抛出异常
        });
    }, std::runtime_error);

    // 验证状态是否保持原样
    std::string result = STM::atomically([&](Transaction& tx) {
        return tx.load(status);
    });

    EXPECT_EQ(result, "Clean");
}

// 3. 并发测试：多线程累加器（验证冲突检测与重试机制）
TEST(STMTest, ConcurrentCounter) {
    STM::Var<int> counter(0);
    
    const int NUM_THREADS = 8;
    const int INC_PER_THREAD = 1000;

    std::vector<std::thread> workers;
    
    // 启动线程
    for (int i = 0; i < NUM_THREADS; ++i) {
        workers.emplace_back([&]() {
            for (int j = 0; j < INC_PER_THREAD; ++j) {
                STM::atomically([&](Transaction& tx) {
                    // 经典的 Read-Modify-Write 竞争场景
                    int val = tx.load(counter);
                    tx.store(counter, val + 1);
                });
            }
        });
    }

    // 等待结束
    for (auto& t : workers) {
        t.join();
    }

    // 验证最终结果
    int final_val = STM::atomically([&](Transaction& tx) {
        return tx.load(counter);
    });

    // 如果没有 MVCC 冲突检测，这个值会小于 8000
    EXPECT_EQ(final_val, NUM_THREADS * INC_PER_THREAD);
}


#include <gtest/gtest.h>
#include <iostream>
#include "MVOSTM/STM.hpp"

// 这是一个手动控制事务步骤的测试
// 目的：精确定位 Validate 为什么返回 True
TEST(Debug, ReproduceLostUpdate) {
    STM::Var<int> x(0);

    // 1. 准备两个事务上下文，模拟两个线程
    TransactionDescriptor desc1;
    Transaction tx1(&desc1);

    TransactionDescriptor desc2;
    Transaction tx2(&desc2);

    // 2. Tx1 开始事务
    tx1.begin();
    int r1 = tx1.load(x); 
    EXPECT_EQ(r1, 0);
    // 此时 Tx1 的 ReadVersion (RV) 假设为 T

    // 3. Tx2 开始事务 (在 Tx1 提交之前)
    tx2.begin();
    int r2 = tx2.load(x);
    EXPECT_EQ(r2, 0);
    // 此时 Tx2 的 RV 也为 T

    // 4. Tx1 修改并提交 -> 成功
    tx1.store(x, 100);
    bool commit1 = tx1.commit();
    EXPECT_TRUE(commit1);
    
    // Checkpoint: 此时 x 的内存中最新版本应该是 100，版本号 > T
    // 我们可以作弊看一下 x 的内部状态（如果方便的话），或者通过原子读取验证
    int current_val = STM::atomically([&](Transaction& tx){ return tx.load(x); });
    EXPECT_EQ(current_val, 100);

    // 5. 【关键步骤】Tx2 尝试修改并提交
    // Tx2 读的时候是 0，现在内存是 100。
    // 根据 TL2/MVCC 规则，Tx2 的 ReadSet 里的 x 已经过期了。
    // 它的 Validate 必须失败！
    tx2.store(x, 200);
    
    // 这一步如果返回 true，说明 Validate 逻辑有 BUG
    bool commit2 = tx2.commit(); 

    if (commit2) {
        std::cout << "\n[CRITICAL FAILURE] Tx2 committed successfully but should have failed!\n";
        std::cout << "Reason: Tx2 read version " << desc2.getReadVersion() 
                  << ", but x was updated by Tx1.\n";
    } else {
        std::cout << "\n[SUCCESS] Tx2 was correctly aborted.\n";
    }

    EXPECT_FALSE(commit2) << "Lost Update Reproduced: Tx2 overwrote Tx1's update without checking!";
}

// 标准 GTest 入口
int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}


