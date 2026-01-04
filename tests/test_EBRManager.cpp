#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <atomic>
#include <new> // for placement new

// 引入你的头文件路径
#include "EBRManager/EBRManager.hpp"
#include "TierAlloc/ThreadHeap/ThreadHeap.hpp"

// ==========================================
// 1. 用于测试的对象
// ==========================================
struct TrackedObject {
    static std::atomic<int> alive_count;
    int value;
    // 填充以避免 Cache False Sharing，让测试更稳定
    uint64_t padding[7]; 

    TrackedObject(int v) : value(v) {
        alive_count.fetch_add(1, std::memory_order_relaxed);
    }

    ~TrackedObject() {
        alive_count.fetch_sub(1, std::memory_order_relaxed);
    }

    // 【工厂方法】确保使用 ThreadHeap 分配内存
    // 因为 EBRManager::retire<T> 内部会调用 ThreadHeap::deallocate
    static TrackedObject* create(int v) {
        // 1. 从 ThreadHeap 申请裸内存
        void* mem = ThreadHeap::allocate(sizeof(TrackedObject));
        if (!mem) throw std::bad_alloc();
        
        // 2. 在该内存上构造对象 (Placement New)
        return new(mem) TrackedObject(v);
    }
};

// 初始化静态计数器
std::atomic<int> TrackedObject::alive_count{0};

// ==========================================
// 2. 测试夹具
// ==========================================
class EBRManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        TrackedObject::alive_count = 0;
    }

    void TearDown() override {
        // 每个测试结束后，尝试清理一下残留
        cleanUpGarbage();
    }

    // 辅助函数：反复进入离开 Epoch 以触发回收
    void cleanUpGarbage() {
        auto* mgr = EBRManager::instance();
        // 尝试多次循环以确保覆盖所有 Epoch 周期
        for(int i = 0; i < 20; ++i) {
            mgr->enter();
            mgr->leave();
            // 让出时间片，给后台清理或别的线程机会
            std::this_thread::yield(); 
            // 如果已经清空，提前退出
            if(TrackedObject::alive_count.load() == 0) break;
        }
    }
};

// ==========================================
// 3. 基础功能测试
// ==========================================

// 测试点：单线程下 allocate -> retire -> reclaim 流程是否通畅
TEST_F(EBRManagerTest, SingleThreadBasicFlow) {
    EBRManager* mgr = EBRManager::instance();

    {
        mgr->enter();
        // 必须使用 create (ThreadHeap::allocate)
        TrackedObject* obj = TrackedObject::create(100);
        EXPECT_EQ(TrackedObject::alive_count.load(), 1);
        
        // 使用模板版本的 retire，它会自动调用 ThreadHeap::deallocate
        mgr->retire(obj);
        
        mgr->leave();
    }

    // 触发回收
    cleanUpGarbage();

    EXPECT_EQ(TrackedObject::alive_count.load(), 0) << "Object should be reclaimed.";
}

// ==========================================
// 4. 多线程压力测试 (SegFault 修复验证)
// ==========================================
TEST_F(EBRManagerTest, MultiThreadStress_ThreadHeap_Integration) {
    EBRManager* mgr = EBRManager::instance();
    
    const int thread_count = 8;            // 线程数
    const int iterations_per_thread = 5000; // 每个线程执行次数 (加大压力)

    std::vector<std::thread> threads;
    
    for (int i = 0; i < thread_count; ++i) {
        threads.emplace_back([&]() {
            // 在子线程获取单例
            EBRManager* local_mgr = EBRManager::instance();
            
            // 重要：某些 ThreadHeap 实现可能需要在每个线程入口显式初始化
            // ThreadHeap::init(); 

            for (int j = 0; j < iterations_per_thread; ++j) {
                local_mgr->enter();
                
                // 1. 分配 (ThreadHeap)
                TrackedObject* obj = TrackedObject::create(j);
                
                // 2. 模拟读写
                obj->value++; 
                
                // 3. 释放 (EBR -> ThreadHeap)
                // 这里调用的是你头文件里的 retire<TrackedObject>
                // 它内部会自动生成调用 ThreadHeap::deallocate 的 deleter
                local_mgr->retire(obj);
                
                local_mgr->leave();
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    // 收尾清理
    cleanUpGarbage();

    // 验证是否有内存泄漏或双重释放导致的计数错误
    int remaining = TrackedObject::alive_count.load();
    if (remaining != 0) {
        printf("Warning: %d objects still alive (Leak or Delayed).\n", remaining);
    }
    
    EXPECT_EQ(remaining, 0);
}

// ==========================================
// 5. 测试自定义 Deleter 接口 (void*)
// ==========================================

// 这是一个特殊测试：验证能不能混用普通的 new/delete (不推荐，但接口允许)
TEST_F(EBRManagerTest, CustomDeleterWithStandardHeap) {
    EBRManager* mgr = EBRManager::instance();

    // 定义一个使用标准 delete 的清理函数
    auto standard_delete = [](void* ptr) {
        TrackedObject* obj = static_cast<TrackedObject*>(ptr);
        delete obj; // 调用标准 delete
    };

    {
        mgr->enter();
        // 这里故意使用标准 new，而不是 ThreadHeap
        TrackedObject* obj = new TrackedObject(999);
        
        // 使用 void* 重载版本，传入自定义 deleter
        mgr->retire(obj, standard_delete);
        
        mgr->leave();
    }

    cleanUpGarbage();
    EXPECT_EQ(TrackedObject::alive_count.load(), 0);
}