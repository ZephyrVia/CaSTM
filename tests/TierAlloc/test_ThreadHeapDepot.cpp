#include <gtest/gtest.h>

#include "ThreadHeap/ChunkHeader.hpp"
#include "ThreadHeap/ThreadHeap.hpp"
#include "ThreadHeap/ThreadHeapDepot.hpp"

#include <atomic>
#include <cstdint>
#include <thread>
#include <unordered_set>
#include <vector>

namespace {
void* AllocateAndTouch(std::size_t size, unsigned char value) {
    void* ptr = ThreadHeap::allocate(size);
    if (ptr) {
        auto* bytes = static_cast<unsigned char*>(ptr);
        bytes[0] = value;
        bytes[size - 1] = value;
    }
    return ptr;
}
}

TEST(ThreadHeapDepotTest, SequentialThreadsReuseOneHeap) {
    std::atomic<bool> ok{true};
    auto worker = [&] {
        void* ptr = AllocateAndTouch(64, 0x42);
        if (!ptr) {
            ok.store(false, std::memory_order_relaxed);
            return;
        }
        ThreadHeap::deallocate(ptr);
    };

    std::thread first(worker);
    first.join();
    const auto before = ThreadHeapDepot::GetInstance().stats();
    ASSERT_TRUE(ok.load(std::memory_order_relaxed));
    ASSERT_GE(before.idle_heaps, 1U);

    for (int i = 0; i < 32; ++i) {
        std::thread thread(worker);
        thread.join();
    }

    const auto after = ThreadHeapDepot::GetInstance().stats();
    EXPECT_TRUE(ok.load(std::memory_order_relaxed));
    EXPECT_EQ(after.created_heaps, before.created_heaps);
    EXPECT_EQ(after.leased_heaps, before.leased_heaps);
    EXPECT_EQ(after.idle_heaps, before.idle_heaps);
    EXPECT_GE(after.reuse_events, before.reuse_events + 32);
    EXPECT_EQ(after.created_heaps, after.leased_heaps + after.idle_heaps);
}

TEST(ThreadHeapDepotTest, LiveObjectSurvivesProducerExitAndHeapHandoff) {
    void* live = nullptr;
    std::thread producer([&] {
        live = AllocateAndTouch(64, 0x5A);
    });
    producer.join();
    ASSERT_NE(live, nullptr);
    EXPECT_EQ(ChunkHeader::Get(live)->type, ChunkHeader::Type::SMALL);
    EXPECT_EQ(static_cast<unsigned char*>(live)[63], 0x5A);

    bool intact = false;
    bool duplicated = false;
    std::thread successor([&] {
        void* fresh = AllocateAndTouch(64, 0x3C);
        duplicated = fresh == live;
        if (fresh) {
            intact = static_cast<unsigned char*>(live)[63] == 0x5A;
            ThreadHeap::deallocate(fresh);
        }
        // The new owner uses the original stable SizeClassPool/Slab.
        ThreadHeap::deallocate(live);
    });
    successor.join();
    EXPECT_FALSE(duplicated);
    EXPECT_TRUE(intact);
}

TEST(ThreadHeapDepotTest, OrphanRemoteFreesAreReclaimedAfterNewLease) {
    // Pin a different owner heap on this thread, so our frees are remote.
    void* guard = AllocateAndTouch(32, 0xCC);
    ASSERT_NE(guard, nullptr);

    std::vector<void*> retired;
    std::thread producer([&] {
        for (int i = 0; i < 512; ++i) {
            void* p = AllocateAndTouch(64, 0x57);
            if (!p) break;
            retired.push_back(p);
        }
    });
    producer.join();
    ASSERT_EQ(retired.size(), 512U);

    for (void* ptr : retired) {
        EXPECT_EQ(static_cast<unsigned char*>(ptr)[63], 0x57);
        ThreadHeap::deallocate(ptr);
    }

    const std::unordered_set<void*> retired_addresses(retired.begin(),
                                                       retired.end());
    bool reused_old_block = false;
    bool succeeded = true;
    std::thread successor([&] {
        std::vector<void*> fresh;
        for (int i = 0; i < 512; ++i) {
            void* p = ThreadHeap::allocate(64);
            if (!p) {
                succeeded = false;
                break;
            }
            reused_old_block |= retired_addresses.count(p) != 0;
            fresh.push_back(p);
        }
        for (void* p : fresh) ThreadHeap::deallocate(p);
    });
    successor.join();
    EXPECT_TRUE(succeeded);
    EXPECT_TRUE(reused_old_block);
    ThreadHeap::deallocate(guard);
}

TEST(ThreadHeapDepotTest, RemoteFreeAndNewOwnerAllocateConcurrently) {
    void* guard = AllocateAndTouch(32, 0xF0);
    ASSERT_NE(guard, nullptr);

    std::vector<void*> old;
    std::thread producer([&] {
        for (int i = 0; i < 256; ++i) {
            void* p = ThreadHeap::allocate(128);
            if (!p) break;
            old.push_back(p);
        }
    });
    producer.join();
    ASSERT_EQ(old.size(), 256U);

    std::atomic<bool> ready{false};
    std::atomic<bool> start{false};
    std::atomic<bool> ok{true};
    std::thread successor([&] {
        void* first = ThreadHeap::allocate(128);
        if (!first) ok.store(false, std::memory_order_relaxed);
        ready.store(true, std::memory_order_release);
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        for (int i = 0; i < 1024; ++i) {
            void* p = ThreadHeap::allocate(128);
            if (!p) {
                ok.store(false, std::memory_order_relaxed);
                break;
            }
            auto* bytes = static_cast<unsigned char*>(p);
            bytes[0] = 0x7F;
            bytes[127] = 0x7F;
            ThreadHeap::deallocate(p);
        }
        ThreadHeap::deallocate(first);
    });
    while (!ready.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    start.store(true, std::memory_order_release);
    for (void* ptr : old) ThreadHeap::deallocate(ptr);
    successor.join();

    EXPECT_TRUE(ok.load(std::memory_order_relaxed));
    ThreadHeap::deallocate(guard);
}

TEST(ThreadHeapDepotTest, FullAndPartialSlabsRemainStableAcrossHandoff) {
    constexpr std::size_t size = 256 * 1024;
    std::vector<void*> live;
    std::thread producer([&] {
        for (int i = 0; i < 9; ++i) {
            void* p = AllocateAndTouch(size, 0x66);
            if (!p) break;
            live.push_back(p);
        }
    });
    producer.join();
    ASSERT_EQ(live.size(), 9U);

    std::unordered_set<std::uintptr_t> slabs;
    for (void* p : live) {
        slabs.insert(reinterpret_cast<std::uintptr_t>(ChunkHeader::Get(p)));
    }
    ASSERT_GE(slabs.size(), 2U);

    bool intact = true;
    std::thread successor([&] {
        void* fresh = AllocateAndTouch(size, 0x77);
        if (!fresh) {
            intact = false;
            return;
        }
        for (void* p : live) {
            auto* bytes = static_cast<unsigned char*>(p);
            if (bytes[0] != 0x66 || bytes[size - 1] != 0x66)
                intact = false;
        }
        ThreadHeap::deallocate(fresh);
        for (void* p : live) ThreadHeap::deallocate(p);
    });
    successor.join();
    EXPECT_TRUE(intact);
}

TEST(ThreadHeapDepotTest, HeapCountStaysBoundedAcrossThreadWaves) {
    constexpr int kConcurrentThreads = 6;
    constexpr int kWaves = 4;
    std::size_t high_water = 0;

    for (int wave = 0; wave < kWaves; ++wave) {
        std::atomic<int> ready{0};
        std::atomic<bool> release{false};
        std::atomic<bool> ok{true};
        std::vector<std::thread> workers;

        for (int i = 0; i < kConcurrentThreads; ++i) {
            workers.emplace_back([&] {
                void* ptr = AllocateAndTouch(64, 0x31);
                if (!ptr) ok.store(false, std::memory_order_relaxed);
                ready.fetch_add(1, std::memory_order_release);
                while (!release.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                ThreadHeap::deallocate(ptr);
            });
        }

        while (ready.load(std::memory_order_acquire) < kConcurrentThreads) {
            std::this_thread::yield();
        }
        const auto during = ThreadHeapDepot::GetInstance().stats();
        EXPECT_GE(during.leased_heaps, static_cast<std::size_t>(kConcurrentThreads));
        release.store(true, std::memory_order_release);
        for (auto& worker : workers) worker.join();
        ASSERT_TRUE(ok.load(std::memory_order_relaxed));

        const auto after = ThreadHeapDepot::GetInstance().stats();
        if (wave == 0) high_water = after.created_heaps;
        else EXPECT_EQ(after.created_heaps, high_water);
        EXPECT_EQ(after.created_heaps, after.leased_heaps + after.idle_heaps);
    }
}
