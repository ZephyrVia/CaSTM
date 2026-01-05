#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <atomic>
#include <cstring> // for memset

// 引入被测模块
#include "TierAlloc/CentralHeap/CentralHeap.hpp"
#include "TierAlloc/common/GlobalConfig.hpp"

class CentralHeapTest : public ::testing::Test {
protected:
    // 便捷访问单例
    CentralHeap& heap = CentralHeap::GetInstance();

    // 辅助检查对齐
    bool IsAligned(void* ptr, size_t alignment) {
        return (reinterpret_cast<uintptr_t>(ptr) & (alignment - 1)) == 0;
    }
};

// 1. 基础功能测试：申请、对齐检查、读写验证
TEST_F(CentralHeapTest, BasicAllocationAndAlignment) {
    void* ptr = heap.fetchChunk();
    
    // 必须分配成功
    ASSERT_NE(ptr, nullptr) << "SystemAllocator failed to allocate memory.";

    // 必须严格 2MB 对齐
    EXPECT_TRUE(IsAligned(ptr, kChunkSize)) 
        << "Ptr " << ptr << " is not aligned to " << kChunkSize;

    // 验证内存可写 (不仅是地址有效，物理页必须可用)
    // 写入 Chunk 头部和尾部
    char* byte_ptr = static_cast<char*>(ptr);
    byte_ptr[0] = 0xAA;
    byte_ptr[kChunkSize - 1] = 0xBB;

    EXPECT_EQ(static_cast<unsigned char>(byte_ptr[0]), 0xAA);
    EXPECT_EQ(static_cast<unsigned char>(byte_ptr[kChunkSize - 1]), 0xBB);

    // 归还
    heap.returnChunk(ptr);
}

// 2. 缓存逻辑测试：验证 LIFO 和 计数器变化
TEST_F(CentralHeapTest, CacheCounterBehavior) {
    size_t initial_count = heap.getFreeChunkCount();

    // 申请一个
    void* ptr1 = heap.fetchChunk();
    size_t count_after_alloc = heap.getFreeChunkCount();

    // 预期：如果缓存原本非空，数量-1；如果原本为空，数量不变(直接找OS要的)
    if (initial_count > 0) {
        EXPECT_EQ(count_after_alloc, initial_count - 1);
    } else {
        EXPECT_EQ(count_after_alloc, 0);
    }

    // 归还一个
    heap.returnChunk(ptr1);
    size_t count_after_free = heap.getFreeChunkCount();

    // 预期：归还后，缓存数量应该+1 (除非触发了水位线，但在单测初期不太可能)
    EXPECT_EQ(count_after_free, count_after_alloc + 1);

    // 再次申请 (验证 LIFO 复用)
    void* ptr2 = heap.fetchChunk();
    // 理论上，刚还回去的 ptr1 应该立即被拿出来 (LIFO)
    // 注意：这不是硬性 API 契约，但在当前实现下应该成立
    EXPECT_EQ(ptr1, ptr2) << "Heap should prioritize the most recently returned chunk (LIFO).";
    
    // 清理现场
    heap.returnChunk(ptr2);
}

// 3. 水位控制测试：验证超过阈值后是否真正 munmap
TEST_F(CentralHeapTest, WaterLevelControl) {
    // 这里的 kMaxCentralCacheSize 需要在 GlobalConfig.hpp 中可见
    // 假设是 512 (1GB)
    const size_t kLimit = kMaxCentralCacheSize;
    
    std::vector<void*> chunks;
    
    // 1. 先把缓存填满 (甚至稍微溢出一点)
    // 为了防止原本里面就有东西，我们只负责 "push" 足够多的数量
    // 但不能凭空 push，必须先 fetch 出来再 return。
    
    // 申请 数量 = Limit + 5
    for (size_t i = 0; i < kLimit + 5; ++i) {
        chunks.push_back(heap.fetchChunk());
    }

    // 2. 全部归还
    for (void* p : chunks) {
        heap.returnChunk(p);
    }

    // 3. 验证
    size_t current_count = heap.getFreeChunkCount();
    
    // 缓存数量不应该超过最大水位线
    EXPECT_LE(current_count, kLimit) 
        << "CentralHeap holding more chunks than kMaxCentralCacheSize limit!";
        
    // 实际上，如果测试开始前缓存是空的，现在应该是 kLimit
    // 溢出的那 5 个应该被 munmap 掉了
}

// 4. 并发压力测试：死锁检测 (Deadlock Detection)
TEST_F(CentralHeapTest, MultiThreadedStressTest) {
    const int kThreadCount = 8;
    const int kOpsPerThread = 1000;
    
    std::atomic<int> success_count{0};
    std::vector<std::thread> threads;

    auto thread_func = [&]() {
        for (int i = 0; i < kOpsPerThread; ++i) {
            // 1. 申请
            void* p = CentralHeap::GetInstance().fetchChunk();
            
            if (p == nullptr) {
                // 这种情况下通常是 mmap 失败 (OOM)，单测中不应发生
                continue;
            }

            // 2. 模拟使用 (写入 dirty 数据，增加 CPU 停留时间，触发竞争)
            std::memset(p, 0xCC, 4096); 

            // 3. 归还
            CentralHeap::GetInstance().returnChunk(p);
            
            success_count++;
        }
    };

    // 启动线程
    for (int i = 0; i < kThreadCount; ++i) {
        threads.emplace_back(thread_func);
    }

    // 等待结束
    for (auto& t : threads) {
        if (t.joinable()) t.join();
    }

    // 验证
    EXPECT_EQ(success_count, kThreadCount * kOpsPerThread) 
        << "Not all operations completed successfully.";
    
    // 还能运行到这里，说明没有发生 Deadlock
    // 此时堆的状态应该是平衡的 (或者有些缓存残留)，但不应崩溃
}