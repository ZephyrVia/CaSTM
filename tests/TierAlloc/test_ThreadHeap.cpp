#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <random>
#include <algorithm>
#include <cstring>
#include <future>

#include "ThreadHeap/ThreadHeap.hpp"
#include "common/SizeClassConfig.hpp"

// 辅助函数：填充并检查内存，防止 Use-After-Free 或 Overwrite
void CheckMemory(void* ptr, size_t size) {
    ASSERT_NE(ptr, nullptr);
    char* data = static_cast<char*>(ptr);
    
    // 写入 Pattern
    for (size_t i = 0; i < size; ++i) {
        data[i] = static_cast<char>(i & 0xFF);
    }

    // 校验 Pattern
    for (size_t i = 0; i < size; ++i) {
        ASSERT_EQ(data[i], static_cast<char>(i & 0xFF));
    }
}

class ThreadHeapTest : public ::testing::Test {
protected:
    // ThreadHeap 是单例，不需要 SetUp/TearDown 创建对象
    // 但我们可以做一些全局清理或状态重置（如果 CentralHeap 支持的话）
};

// 1. 基础小对象分配 (Hot Path)
TEST_F(ThreadHeapTest, SmallAllocation_Basic) {
    // 测试几个典型大小：8B (Min), 64B, 256B, 4KB (Page)
    std::vector<size_t> sizes = {8, 64, 256, 1024, 4096};

    for (size_t size : sizes) {
        void* ptr = ThreadHeap::allocate(size);
        EXPECT_NE(ptr, nullptr) << "Failed to allocate size: " << size;
        
        CheckMemory(ptr, size);
        
        ThreadHeap::deallocate(ptr);
    }
}

// 2. 边界条件测试
TEST_F(ThreadHeapTest, SmallAllocation_Boundaries) {
    // 最大小对象
    size_t max_small = SizeClassConfig::kMaxAlloc; 
    void* ptr = ThreadHeap::allocate(max_small);
    EXPECT_NE(ptr, nullptr);
    CheckMemory(ptr, max_small);
    ThreadHeap::deallocate(ptr);

    // 最小对象
    void* ptr_tiny = ThreadHeap::allocate(1); // 应该向上取整到 8
    EXPECT_NE(ptr_tiny, nullptr);
    CheckMemory(ptr_tiny, 1);
    ThreadHeap::deallocate(ptr_tiny);
}

// 3. 大对象分配 (Span Path, > 256KB)
// 注意：这需要 CentralHeap 正确工作
TEST_F(ThreadHeapTest, LargeAllocation_Span) {
    size_t large_size = SizeClassConfig::kMaxAlloc + 1024; // 256KB + 1KB
    
    void* ptr = ThreadHeap::allocate(large_size);
    ASSERT_NE(ptr, nullptr);
    
    // 验证是否是大对象
    auto* header = ChunkHeader::Get(ptr);
    EXPECT_EQ(header->type, ChunkHeader::Type::LARGE);

    CheckMemory(ptr, large_size);
    
    ThreadHeap::deallocate(ptr);
}

// 4. 核心功能：跨线程释放 (Remote Free)
TEST_F(ThreadHeapTest, CrossThreadDeallocation) {
    const int alloc_count = 100;
    std::vector<void*> ptrs(alloc_count);

    // 步骤 1: 主线程 (Thread A) 分配
    for (int i = 0; i < alloc_count; ++i) {
        ptrs[i] = ThreadHeap::allocate(64); // 64字节小对象
        CheckMemory(ptrs[i], 64);
    }

    // 步骤 2: 子线程 (Thread B) 释放
    std::thread t([&ptrs]() {
        for (void* ptr : ptrs) {
            // 这里应该触发 slab->freeRemote(ptr)
            // 因为 Slab 的 owner 是主线程，而当前是子线程
            ThreadHeap::deallocate(ptr);
        }
    });

    t.join();

    // 步骤 3: 验证主线程并未崩溃，且能继续分配
    // 注意：由于 Remote Free 只是把内存挂在 AtomicList 上，
    // 主线程下次分配时应该能回收这些内存（通过 ReclaimRemote）。
    
    void* p_new = ThreadHeap::allocate(64);
    EXPECT_NE(p_new, nullptr);
    ThreadHeap::deallocate(p_new);
}

// 5. 生产者-消费者模型 (高并发模拟)
TEST_F(ThreadHeapTest, ProducerConsumer_Stress) {
    const int num_items = 1000;
    std::vector<void*> shared_queue;
    std::mutex mtx;

    // 生产者线程：只管分配
    auto producer = std::async(std::launch::async, [&]() {
        for (int i = 0; i < num_items; ++i) {
            void* p = ThreadHeap::allocate(128); // 128B
            // 简单的写入验证
            *static_cast<int*>(p) = i; 
            
            std::lock_guard<std::mutex> lock(mtx);
            shared_queue.push_back(p);
        }
    });

    // 消费者线程：只管释放
    auto consumer = std::async(std::launch::async, [&]() {
        int freed_count = 0;
        while (freed_count < num_items) {
            void* p = nullptr;
            {
                std::lock_guard<std::mutex> lock(mtx);
                if (!shared_queue.empty()) {
                    p = shared_queue.back();
                    shared_queue.pop_back();
                }
            }

            if (p) {
                // 验证数据完整性
                // int val = *static_cast<int*>(p); 
                ThreadHeap::deallocate(p);
                freed_count++;
            } else {
                std::this_thread::yield();
            }
        }
    });

    producer.get();
    consumer.get();
}

// 6. 随机大小压力测试 (Fuzzing)
TEST_F(ThreadHeapTest, RandomSizeStress) {
    std::vector<void*> ptrs;
    std::mt19937 rng(42);
    // 随机大小：8B 到 1MB
    std::uniform_int_distribution<size_t> dist_size(8, 1024 * 1024);
    
    const int ops = 5000;

    for (int i = 0; i < ops; ++i) {
        size_t sz = dist_size(rng);
        void* p = ThreadHeap::allocate(sz);
        EXPECT_NE(p, nullptr);
        
        // 写入头部和尾部，检测越界
        char* data = static_cast<char*>(p);
        data[0] = 0; 
        data[sz - 1] = 0;

        ptrs.push_back(p);
    }

    // 乱序释放
    std::shuffle(ptrs.begin(), ptrs.end(), rng);
    for (void* p : ptrs) {
        ThreadHeap::deallocate(p);
    }
}