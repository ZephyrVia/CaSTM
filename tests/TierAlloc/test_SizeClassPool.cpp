#include <gtest/gtest.h>
#include <vector>
#include <cstring> // for memset
#include "ThreadHeap/SizeClassPool.hpp"
#include "ThreadHeap/ThreadChunkCache.hpp"
#include "ThreadHeap/Slab.hpp"


class SizeClassPoolTest : public ::testing::Test {
protected:
    void SetUp() override {
        // 每个测试开始前，确保有一个干净的 Cache
        cache_ = new ThreadChunkCache();
    }

    void TearDown() override {
        delete cache_;
    }

    ThreadChunkCache* cache_;
};

// 1. 基础分配与释放测试
TEST_F(SizeClassPoolTest, BasicAllocAndFree) {
    // 修正：两段式初始化
    SizeClassPool pool;
    pool.Init(64, cache_);

    void* p1 = pool.allocate();
    EXPECT_NE(p1, nullptr);
    
    // 写入数据测试，确保内存可用
    std::memset(p1, 0xAA, 64);

    void* p2 = pool.allocate();
    EXPECT_NE(p2, nullptr);
    EXPECT_NE(p1, p2);

    // 获取 Slab (Slab::GetSlab 位于 Slab.hpp，是静态方法)
    Slab* slab = Slab::GetSlab(p1); 
    
    // 验证释放
    pool.deallocate(slab, p1);
    pool.deallocate(slab, p2);
}

// 2. 验证 LIFO 策略 (Partial List)
TEST_F(SizeClassPoolTest, LIFO_Reuse_Strategy) {
    // 修正：两段式初始化
    SizeClassPool pool;
    pool.Init(128, cache_);

    void* p1 = pool.allocate();
    void* p2 = pool.allocate();
    void* p3 = pool.allocate();

    // 释放 p3 (最后分配的)
    Slab* slab = Slab::GetSlab(p3);
    pool.deallocate(slab, p3);

    // 再次分配，根据 LIFO，应该立刻拿回 p3 (或者在同一个 Slab 内的新地址)
    void* p4 = pool.allocate();
    EXPECT_NE(p4, nullptr);
    
    // 验证确实复用了同一个 Slab
    EXPECT_EQ(Slab::GetSlab(p4), slab);
}

// 3. 验证 Full List 转换与自动扩容
TEST_F(SizeClassPoolTest, SlabExhaustionAndNewSlab) {
    size_t large_size = 256 * 1024; 
    
    // 修正：两段式初始化
    SizeClassPool pool;
    pool.Init(large_size, cache_);
    
    std::vector<void*> ptrs;
    
    // 1. 填满第一个 Slab
    void* first_ptr = pool.allocate();
    ptrs.push_back(first_ptr);
    Slab* first_slab = Slab::GetSlab(first_ptr);

    bool new_slab_created = false;
    for (int i = 0; i < 20; ++i) { 
        void* p = pool.allocate();
        ptrs.push_back(p);
        Slab* current_slab = Slab::GetSlab(p);
        
        if (current_slab != first_slab) {
            new_slab_created = true;
            break;
        }
    }

    EXPECT_TRUE(new_slab_created) << "Pool should allocate new slab when current is full";

    // 清理
    for (void* p : ptrs) {
        Slab* s = Slab::GetSlab(p);
        pool.deallocate(s, p); 
    }
}

TEST_F(SizeClassPoolTest, RescueFromFullList) {
    size_t block_size = 512 * 1024; // 512KB
    
    // 修正：两段式初始化
    SizeClassPool pool;
    pool.Init(block_size, cache_);
    
    std::vector<void*> full_slab_ptrs;

    // 1. 填满第一个 Slab (Slab A)
    void* p0 = pool.allocate();
    full_slab_ptrs.push_back(p0);
    Slab* slab_a = Slab::GetSlab(p0);
    
    while (true) {
        void* p = pool.allocate();
        if (Slab::GetSlab(p) != slab_a) {
            // 存下新 Slab 的第一个指针以便后续释放，防止内存泄漏
            // 但在这个测试里我们主要关注 Rescue
            // 简单起见，不追踪这个新 Slab 的所有指针
            break;
        }
        full_slab_ptrs.push_back(p);
    }

    // 2. 模拟远程释放 (Remote Free to Slab A)
    // 释放 Slab A 中的第一个块
    void* victim_ptr = full_slab_ptrs[0]; 
    slab_a->freeRemote(victim_ptr);

    // 3. 耗尽 Current (Slab B) 并期待 Rescue
    bool rescued = false;
    void* rescued_ptr = nullptr;

    for (int i = 0; i < 10; ++i) {
        void* p = pool.allocate();
        if (Slab::GetSlab(p) == slab_a) {
            rescued = true;
            rescued_ptr = p;
            break; 
        }
    }

    EXPECT_TRUE(rescued) << "Allocator should eventually rescue memory from Slab A in full list";
}