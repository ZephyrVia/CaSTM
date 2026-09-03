#include <gtest/gtest.h>
#include <condition_variable>
#include <cstddef>
#include <mutex>
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

    // 【工厂方法】确保使用 ThreadHeap 分配内存。
    // 约定：走 EBRManager::retire<T> 默认 deleter 的类型若使用 ThreadHeap，
    // 必须提供类内 operator new/delete（与 Occ 的 VersionNode 相同），
    // delete 才能正确分派回 ThreadHeap::deallocate。
    static void* operator new(size_t size) {
        return ThreadHeap::allocate(size);
    }
    static void operator delete(void* p) {
        ThreadHeap::deallocate(p);
    }

    static TrackedObject* create(int v) {
        return new TrackedObject(v);
    }
};

// 初始化静态计数器
std::atomic<int> TrackedObject::alive_count{0};

// 使用系统堆分配，避免这个协议测试同时依赖 ThreadHeap 的回收路径。
struct EpochProbe {
    explicit EpochProbe(std::atomic<int>& destruction_count)
        : destruction_count_(destruction_count) {}

    ~EpochProbe() {
        destruction_count_.fetch_add(1, std::memory_order_relaxed);
    }

    std::atomic<int>& destruction_count_;
};

// C++17 没有 std::latch；这个单调 phase gate 用于精确排列 EBR 事件，
// 不依赖 sleep 或调度时序。
class PhaseGate {
public:
    void signal() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++phase_;
        }
        condition_.notify_all();
    }

    void wait_until(int target) {
        std::unique_lock<std::mutex> lock(mutex_);
        condition_.wait(lock, [this, target] { return phase_ >= target; });
    }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    int phase_ = 0;
};

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

// 回归：线程持有旧的 announced epoch E 时，全局纪元可能已经推进到 E+1。
// 退休对象必须按实际全局纪元入桶；否则旧代码会在 E+2 收集 E 桶，
// 而仍处于 E+1 的读者尚未离开临界区。
TEST_F(EBRManagerTest, RetireUsesGlobalEpochWhenOwnerSlotIsBehind) {
    EBRManager* mgr = EBRManager::instance();
    std::atomic<int> destruction_count{0};
    std::atomic<EpochProbe*> observed{nullptr};
    EpochProbe* victim = new EpochProbe(destruction_count);

    PhaseGate owner_entered;
    PhaseGate allow_owner_retire;
    PhaseGate reader_entered;
    PhaseGate retired;
    PhaseGate allow_reader_leave;

    // T1 stays announced at E while the coordinator advances the global
    // epoch to E+1, then retires the object at that global epoch.
    std::thread owner([&] {
        mgr->enter();
        owner_entered.signal();
        allow_owner_retire.wait_until(1);

        mgr->retire(victim);
        retired.signal();
        mgr->leave();
    });

    owner_entered.wait_until(1);

    // T1's active E slot does not block E -> E+1: at the current epoch E,
    // the EBR advance rule only rejects active slots strictly older than E.
    std::thread advance_to_e1([&] {
        mgr->enter();
        mgr->leave();
    });
    advance_to_e1.join();

    // T2 enters at E+1 and obtains the pointer before T1 retires it.
    std::thread reader([&] {
        mgr->enter();
        observed.store(victim, std::memory_order_release);
        reader_entered.signal();
        allow_reader_leave.wait_until(1);
        mgr->leave();
    });
    reader_entered.wait_until(1);

    allow_owner_retire.signal();
    retired.wait_until(1);
    owner.join();

    // T1's leave now advances E+1 -> E+2 and collects bucket E. The old
    // implementation incorrectly placed victim in bucket E; the fixed
    // implementation places it in bucket E+1, so T2 still protects it here.

    EXPECT_EQ(observed.load(std::memory_order_acquire), victim);
    EXPECT_EQ(destruction_count.load(std::memory_order_relaxed), 0)
        << "an active E+1 reader must prevent reclaim at E+2";

    allow_reader_leave.signal();
    reader.join();

    // Once T2 leaves, one more deterministic leave supplies the next grace
    // period and collects the E+1 bucket.
    std::thread advance_after_reader([&] {
        mgr->enter();
        mgr->leave();
    });
    advance_after_reader.join();

    EXPECT_EQ(destruction_count.load(std::memory_order_relaxed), 1);
}

// 边界 A：退休线程的 announced epoch 落后一代时，必须使用退休瞬间的
// 全局纪元。这里没有读者参与，单纯观察 E+2 是否错误收集了 E 桶，
// 从而把“入 E+1 桶”这一点与上面的 reader 回归分开验证。
TEST_F(EBRManagerTest, RetireAtLaggingSlotUsesCurrentGlobalEpoch) {
    EBRManager* mgr = EBRManager::instance();
    std::atomic<int> destruction_count{0};
    EpochProbe* victim = new EpochProbe(destruction_count);

    PhaseGate owner_entered;
    PhaseGate allow_retire;

    std::thread owner([&] {
        mgr->enter();
        owner_entered.signal();
        allow_retire.wait_until(1);
        mgr->retire(victim);
        mgr->leave();
    });

    owner_entered.wait_until(1);

    // Keep the owner announced at E while another thread advances G to E+1.
    std::thread advance_to_e1([&] {
        mgr->enter();
        mgr->leave();
    });
    advance_to_e1.join();

    allow_retire.signal();
    owner.join();

    // Owner leave advances E+1 -> E+2. A victim tagged with E would be
    // reclaimed now; a victim tagged with the actual global E+1 must remain.
    EXPECT_EQ(destruction_count.load(std::memory_order_relaxed), 0);

    std::thread advance_to_e3([&] {
        mgr->enter();
        mgr->leave();
    });
    advance_to_e3.join();

    EXPECT_EQ(destruction_count.load(std::memory_order_relaxed), 1);
}

// 边界 B：槽位 epoch 与全局 epoch 相同时，仍应按该全局 epoch 入桶，
// 两次确定性的纪元推进后即可回收，不应被错误地再延迟一个轮次。
TEST_F(EBRManagerTest, RetireAtCurrentGlobalEpochUsesNormalGracePeriod) {
    EBRManager* mgr = EBRManager::instance();
    std::atomic<int> destruction_count{0};
    EpochProbe* victim = new EpochProbe(destruction_count);

    std::thread retire_at_current_epoch([&] {
        mgr->enter();
        mgr->retire(victim);
        mgr->leave();
    });
    retire_at_current_epoch.join();

    // The retire thread advances G -> G+1 on leave, but bucket G is collected
    // only after the next advance to G+2.
    EXPECT_EQ(destruction_count.load(std::memory_order_relaxed), 0);

    std::thread advance_to_collection([&] {
        mgr->enter();
        mgr->leave();
    });
    advance_to_collection.join();

    EXPECT_EQ(destruction_count.load(std::memory_order_relaxed), 1);
}
