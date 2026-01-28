#include <gtest/gtest.h>
#include <thread>
#include <atomic>
#include <vector>

// 包含你的头文件
#include "WwSTM/TxContext.hpp"
#include "WwSTM/TMVar.hpp"

using namespace STM::Ww;

// ==========================================
// 1. 定义链表节点结构
// ==========================================
struct ListNode {
    int val;
    TMVar<ListNode>* next; // 指向下一个被 TMVar 包装的节点

    // 构造函数，方便 emplace/alloc
    ListNode(int v = 0, TMVar<ListNode>* n = nullptr) 
        : val(v), next(n) {}
};

class TxLinkedListTest : public ::testing::Test {
protected:
    void SetUp() override {
        // 如果有单例需要重置，在这里调用
    }
    void TearDown() override {}
};

// ==========================================
// 测试案例 1：单线程链表追加 (优化版 - O(1) 插入)
// ==========================================
TEST_F(TxLinkedListTest, SingleThreadAppend) {
    TMVar<ListNode>* head_ptr = nullptr;
    // 本地缓存尾指针，避免每次从头遍历
    TMVar<ListNode>* tail_ptr = nullptr;

    // 1. 初始化
    {
        TxContext ctx;
        head_ptr = ctx.alloc<ListNode>(-1, nullptr);
        tail_ptr = head_ptr; // 初始时，头也是尾
        ASSERT_TRUE(ctx.commit());
    }

    const int loop_count = 1000; // 插入 1000 个节点

    // 2. 循环追加节点
    for (int i = 0; i < loop_count; ++i) {
        TxContext ctx;
        bool committed = false;
        
        while (!committed) {
            ctx.begin();

            // 直接读取上一次已知的尾节点
            // 注意：在单线程中 tail_ptr 永远是最新的。
            // 但在 STM 语义中，我们需要通过 read 获取数据的拷贝。
            ListNode tail_data = ctx.read(tail_ptr);

            // 再次确认确实是尾部 (next == nullptr)
            if (tail_data.next == nullptr) {
                // A. 分配新节点
                TMVar<ListNode>* new_node = ctx.alloc<ListNode>(i, nullptr);

                // B. 链接：修改旧尾部的副本
                tail_data.next = new_node;
                ctx.write(tail_ptr, tail_data);

                // C. 提交
                if (ctx.commit()) {
                    // 提交成功，更新本地 tail_ptr，供下一次循环使用
                    tail_ptr = new_node; 
                    committed = true;
                }
            } else {
                // 防御性编程：理论上单线程不会进这里
                // 如果进这里说明 tail_ptr 过期了，向前移动
                tail_ptr = tail_data.next;
                // 不需要 commit，直接 continue 重试
            }
        }
    }

    // 3. 验证链表完整性
    {
        TxContext verify_ctx;
        
        TMVar<ListNode>* curr_var = head_ptr;
        ListNode head_data = verify_ctx.read(curr_var);
        
        ASSERT_EQ(head_data.val, -1);
        curr_var = head_data.next;

        int count = 0;
        while (curr_var != nullptr) {
            ListNode data = verify_ctx.read(curr_var);
            ASSERT_EQ(data.val, count); // 验证顺序：0, 1, 2...
            curr_var = data.next;
            count++;
        }

        ASSERT_TRUE(verify_ctx.commit());
        std::printf("[SingleThread] Final List Length: %d (Expected: %d)\n", count, loop_count);
        ASSERT_EQ(count, loop_count);
    }
}

// ==========================================
// 测试案例 2：双线程并发追加 (低压力)
// ==========================================
TEST_F(TxLinkedListTest, TwoThreadsConcurrentAppend) {
    TMVar<ListNode>* head_ptr = nullptr;
    
    // 全局尾部提示 (Hint)，用于加速并发下的查找
    // 这是一个原子指针，指向大概率是尾部的节点
    std::atomic<TMVar<ListNode>*> tail_hint{nullptr};

    // 1. 初始化
    {
        TxContext ctx;
        head_ptr = ctx.alloc<ListNode>(-1, nullptr);
        tail_hint.store(head_ptr);
        ASSERT_TRUE(ctx.commit());
    }

    const int nodes_per_thread = 50; // 每个线程插入 50 个，总共 100 个
    
    auto thread_func = [&](int thread_id) {
        TxContext ctx;
        for(int i = 0; i < nodes_per_thread; ++i) {
            bool committed = false;
            while(!committed) {
                ctx.begin();
                
                // 1. 获取搜索起点 (从 Hint 开始，避免从 head 开始的 O(N^2))
                TMVar<ListNode>* curr_ptr = tail_hint.load(std::memory_order_acquire);
                
                // 2. 寻找真正的尾部 (因为并发，Hint 可能稍微滞后)
                while(true) {
                    ListNode curr_data = ctx.read(curr_ptr);
                    if(curr_data.next == nullptr) {
                        // 找到了尾部，执行插入
                        TMVar<ListNode>* new_node = ctx.alloc<ListNode>(thread_id * 1000 + i, nullptr); // 值包含线程ID以便调试
                        
                        curr_data.next = new_node;
                        ctx.write(curr_ptr, curr_data);
                        
                        if(ctx.commit()) {
                            // 提交成功，尝试更新全局 Hint
                            // 即使 compare_exchange 失败也没关系，说明别人更新得更快
                            TMVar<ListNode>* expected = curr_ptr;
                            tail_hint.compare_exchange_strong(expected, new_node);
                            committed = true;
                        }
                        break; // 跳出内部 while
                    } else {
                        // 还没到尾部，继续向后
                        curr_ptr = curr_data.next;
                    }
                    
                    // 检查事务是否依然有效 (Wound-Wait 优化)
                    if(!ctx.isActive()) break; 
                }
                // 如果 commit 失败或事务被 Wound，外层 while 会重试
            }
        }
    };

    // 3. 启动线程
    std::thread t1(thread_func, 1);
    std::thread t2(thread_func, 2);
    std::thread t3(thread_func, 3);
    std::thread t4(thread_func, 4);
    
    t1.join();
    t2.join();
    t3.join();
    t4.join();

    // 4. 验证结果
    {
        TxContext verify_ctx;
        int count = 0;
        TMVar<ListNode>* curr = head_ptr;
        
        // 跳过 dummy head
        ListNode node = verify_ctx.read(curr);
        curr = node.next;

        while(curr != nullptr) {
            count++;
            ListNode d = verify_ctx.read(curr);
            curr = d.next;
        }
        verify_ctx.commit();

        std::printf("[Concurrent] Final List Length: %d (Expected: %d)\n", count, nodes_per_thread * 2);
        ASSERT_EQ(count, nodes_per_thread * 4);
    }
}

