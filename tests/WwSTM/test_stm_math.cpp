#include <gtest/gtest.h>
#include <thread>
#include <atomic>
#include <vector>

// 包含你的头文件
#include "WwSTM/TxContext.hpp"
#include "WwSTM/TMVar.hpp"

using namespace STM::Ww;

class TxContextTest : public ::testing::Test {
protected:
    void SetUp() override {
        // 如果你的 GlobalClock 或 EBRManager 需要显式初始化，请在这里调用
    }

    void TearDown() override {
        // 清理工作
    }
};

// ==========================================
// 测试案例：双线程并发累加
// ==========================================
TEST_F(TxContextTest, TwoThreadsConcurrentIncrement) {
    // 1. 准备阶段：主线程分配共享变量
    TMVar<int>* shared_counter = nullptr;
    
    {
        TxContext main_ctx; // 构造函数自动开启一个事务
        // 使用 TxContext 分配内存 (ThreadHeap)
        shared_counter = main_ctx.alloc<int>(0);
        
        // 必须提交，否则 shared_counter 在 main_ctx 析构时会被视为未提交的垃圾回收掉
        ASSERT_TRUE(main_ctx.commit()); 
    }

    // 此时 shared_counter 已经持久化（Stable），版本号已更新

    // 2. 定义工作线程逻辑
    // 每个线程的目标是将计数器增加 loop_count 次
    const int loop_count = 1000;
    
    auto thread_task = [&](int thread_id) {
        TxContext ctx; // 每个线程拥有独立的 TxContext

        for (int i = 0; i < loop_count; ++i) {
            bool committed = false;
            while (!committed) {
                // 每次循环开始前重置事务状态
                // 虽然构造函数开启了事务，但为了处理 Retry，我们在循环头显式调用 begin
                ctx.begin(); 

                // --- 事务操作开始 ---
                
                // 1. 读取
                int val = ctx.read(shared_counter);

                // 检查是否被 Wound (被更老的事务杀死了)
                if (!ctx.isActive()) {
                    // 只要状态不对，commit 就会失败并自动清理，这里直接 continue 重试即可
                    continue; 
                }

                // 2. 写入
                ctx.write(shared_counter, val + 1);

                // --- 事务操作结束 ---

                // 3. 提交
                committed = ctx.commit();
                
                // 如果提交失败 (committed == false)，TxContext 会自动调用 abortTransaction() 清理
                // while 循环会再次通过 ctx.begin() 开启新事务
            }
        }
    };

    // 3. 启动两个线程
    std::thread t1(thread_task, 1);
    std::thread t2(thread_task, 2);
    std::thread t3(thread_task, 3);
    std::thread t4(thread_task, 4);

    t1.join();
    t2.join();
    t3.join();
    t4.join();
    

    // 4. 验证结果
    {
        TxContext verify_ctx;
        int final_val = verify_ctx.read(shared_counter);
        ASSERT_TRUE(verify_ctx.commit());

        // 预期结果：0 + 100 + 100 = 200
        std::printf("Final Value: %d (Expected: %d)\n", final_val, loop_count * 4);
        ASSERT_EQ(final_val, loop_count * 4);
    }
    
    // 注意：shared_counter 的内存由 ThreadHeap 管理，
    // 在真实应用中通常不需要手动 delete TMVar，而是由容器或析构逻辑处理。
    // 在测试中，程序结束时 ThreadHeap 会清理。
}

