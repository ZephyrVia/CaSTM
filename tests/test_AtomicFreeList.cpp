#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <atomic>

// 包含你的头文件
#include "common/AtomicFreeList.hpp"

// 一个用于测试的辅助结构体
// 必须保证前 8 字节（64位系统）可以被 AtomicFreeList 覆盖作为 next 指针
struct TestBlock {
    AtomicFreeList::Node* next; // 占位符，AtomicFreeList 会修改这个
    int id;                     // 业务数据，用于校验
    char padding[50];           // 模拟真实 payload
};

class AtomicFreeListTest : public ::testing::Test {
protected:
    AtomicFreeList list;

    // 辅助函数：遍历链表计算节点数量
    size_t CountNodes(void* head) {
        size_t count = 0;
        auto* node = static_cast<AtomicFreeList::Node*>(head);
        while (node) {
            count++;
            node = node->next;
        }
        return count;
    }

    // 辅助函数：将 void* 链表转为 vector 以便验证顺序
    std::vector<int> ListToIds(void* head) {
        std::vector<int> ids;
        auto* node = static_cast<AtomicFreeList::Node*>(head);
        while (node) {
            // 强转回 TestBlock 获取 id
            // 注意：AtomicFreeList::Node 的布局正好对应 TestBlock 的起始位置
            TestBlock* block = reinterpret_cast<TestBlock*>(node);
            ids.push_back(block->id);
            node = node->next;
        }
        return ids;
    }
};

// 1. 基础功能测试：单个 Push 和 Steal
TEST_F(AtomicFreeListTest, BasicPushAndSteal) {
    TestBlock block1{nullptr, 100};
    
    // 初始状态应为空
    EXPECT_EQ(list.steal_all(), nullptr);

    // Push 一个
    list.push(&block1);

    // Steal 回来
    void* stolen = list.steal_all();
    EXPECT_EQ(stolen, &block1);

    // 再次 Steal 应为空
    EXPECT_EQ(list.steal_all(), nullptr);
}

// 2. 顺序测试：验证 LIFO (栈) 行为
TEST_F(AtomicFreeListTest, LIFOOrder) {
    TestBlock b1{nullptr, 1};
    TestBlock b2{nullptr, 2};
    TestBlock b3{nullptr, 3};

    list.push(&b1);
    list.push(&b2);
    list.push(&b3);

    // 期望顺序: 3 -> 2 -> 1
    void* head = list.steal_all();
    std::vector<int> result = ListToIds(head);

    ASSERT_EQ(result.size(), 3);
    EXPECT_EQ(result[0], 3);
    EXPECT_EQ(result[1], 2);
    EXPECT_EQ(result[2], 1);
}

// 3. 边界测试：Push nullptr
TEST_F(AtomicFreeListTest, PushNullptrSafe) {
    list.push(nullptr);
    EXPECT_EQ(list.steal_all(), nullptr);
    
    TestBlock b1{nullptr, 1};
    list.push(&b1);
    list.push(nullptr); // 应该被忽略
    
    void* head = list.steal_all();
    EXPECT_EQ(head, &b1);
    EXPECT_EQ(static_cast<AtomicFreeList::Node*>(head)->next, nullptr);
}

// 4. 并发测试：多生产者 (Multi-Producer)
TEST_F(AtomicFreeListTest, MultiThreadedPush) {
    const int kThreads = 8;
    const int kItemsPerThread = 10000;
    const int kTotalItems = kThreads * kItemsPerThread;

    // 预分配所有内存块，避免测试中 new 的干扰
    std::vector<TestBlock> all_blocks(kTotalItems);
    
    std::vector<std::thread> threads;
    std::atomic<bool> start_flag{false};

    // 启动多个线程进行 Push
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&, i]() {
            // 等待开始信号，尽量让线程同时运行
            while (!start_flag.load(std::memory_order_relaxed));

            int start_idx = i * kItemsPerThread;
            int end_idx = start_idx + kItemsPerThread;

            for (int j = start_idx; j < end_idx; ++j) {
                all_blocks[j].id = j; // 设置唯一ID
                list.push(&all_blocks[j]);
            }
        });
    }

    // 开始！
    start_flag.store(true);

    // 等待所有 Push 完成
    for (auto& t : threads) {
        t.join();
    }

    // 窃取所有节点
    void* head = list.steal_all();
    
    // 验证数量正确
    size_t count = CountNodes(head);
    EXPECT_EQ(count, kTotalItems) << "Lost nodes during concurrent push!";

    // 验证数据完整性 (确保没有环，且所有 ID 都在)
    std::vector<bool> id_check(kTotalItems, false);
    auto* node = static_cast<AtomicFreeList::Node*>(head);
    int nodes_traversed = 0;
    
    while (node) {
        TestBlock* block = reinterpret_cast<TestBlock*>(node);
        ASSERT_GE(block->id, 0);
        ASSERT_LT(block->id, kTotalItems);
        
        // 确保每个 ID 只出现一次 (防止链表成环或重复)
        EXPECT_FALSE(id_check[block->id]) << "Duplicate ID found: " << block->id;
        id_check[block->id] = true;

        node = node->next;
        nodes_traversed++;
        
        // 防止死循环 (如果有环)
        if (nodes_traversed > kTotalItems) {
            FAIL() << "Infinite loop detected in linked list!";
        }
    }
}

// 5. 并发测试：边推边偷 (Concurrent Push & Steal)
// 模拟 MPSC 真实场景：多个线程还内存，Owner 线程不定期回收
TEST_F(AtomicFreeListTest, ConcurrentPushAndSteal) {
    const int kProducers = 4;
    const int kPushCount = 50000;
    std::atomic<int> total_stolen{0};
    std::atomic<bool> producers_done{false};

    std::vector<TestBlock> blocks(kProducers * kPushCount);

    // 消费者 (Stealer) 线程
    std::thread consumer([&]() {
        while (!producers_done.load(std::memory_order_acquire) || list.steal_all() != nullptr) {
            void* batch = list.steal_all();
            if (batch) {
                total_stolen.fetch_add(CountNodes(batch));
            }
            std::this_thread::yield(); // 让出一点 CPU 给生产者
            
            // double check logic: 如果生产者完了，这里还需要最后一次 steal_all 
            // 上面的循环条件已经涵盖了大部分情况，但为了严谨：
            if (producers_done.load(std::memory_order_acquire)) {
                 void* last_batch = list.steal_all();
                 if (last_batch) total_stolen.fetch_add(CountNodes(last_batch));
                 break;
            }
        }
    });

    // 生产者线程组
    std::vector<std::thread> producers;
    for (int i = 0; i < kProducers; ++i) {
        producers.emplace_back([&, i]() {
            int start = i * kPushCount;
            for (int j = 0; j < kPushCount; ++j) {
                list.push(&blocks[start + j]);
            }
        });
    }

    for (auto& t : producers) t.join();
    producers_done.store(true, std::memory_order_release);
    
    consumer.join();

    EXPECT_EQ(total_stolen.load(), kProducers * kPushCount) 
        << "Total stolen items mismatch. Leak occurred or thread sync issue.";
}