#include <gtest/gtest.h>

#include "ThreadHeap/ChunkHeader.hpp"
#include "ThreadHeap/MediumArena.hpp"
#include "ThreadHeap/ThreadHeap.hpp"
#include "common/SizeClassConfig.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <future>
#include <limits>
#include <thread>
#include <vector>

namespace {

std::uintptr_t ChunkBase(void* ptr) {
    return reinterpret_cast<std::uintptr_t>(ptr) &
           static_cast<std::uintptr_t>(kChunkMask);
}

}  // namespace

TEST(MediumArenaTest, RoutesSizeBoundaries) {
    void* small = ThreadHeap::allocate(SizeClassConfig::kMaxAlloc);
    ASSERT_NE(small, nullptr);
    EXPECT_EQ(ChunkHeader::Get(small)->type, ChunkHeader::Type::SMALL);
    ThreadHeap::deallocate(small);

    void* medium = ThreadHeap::allocate(SizeClassConfig::kMaxAlloc + 1);
    ASSERT_NE(medium, nullptr);
    EXPECT_EQ(ChunkHeader::Get(medium)->type, ChunkHeader::Type::MEDIUM);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(medium) %
                  MediumArena::kPageSize, 0U);
    ThreadHeap::deallocate(medium);

    void* largest = ThreadHeap::allocate(MediumArena::kMaxAlloc);
    ASSERT_NE(largest, nullptr);
    EXPECT_EQ(ChunkHeader::Get(largest)->type, ChunkHeader::Type::MEDIUM);
    auto* last_byte = static_cast<unsigned char*>(largest) +
                      MediumArena::kMaxAlloc - 1;
    *last_byte = 0xA5;
    EXPECT_EQ(*last_byte, 0xA5);
    ThreadHeap::deallocate(largest);

    void* large = ThreadHeap::allocate(MediumArena::kMaxAlloc + 1);
    ASSERT_NE(large, nullptr);
    EXPECT_EQ(ChunkHeader::Get(large)->type, ChunkHeader::Type::LARGE);
    ThreadHeap::deallocate(large);
}

TEST(MediumArenaTest, PacksAndReusesPagesWithoutTouchingLiveObjects) {
    constexpr std::size_t a_size = 300 * 1024;
    constexpr std::size_t b_size = 500 * 1024;
    constexpr std::size_t c_size = 700 * 1024;

    void* a = ThreadHeap::allocate(a_size);
    void* b = ThreadHeap::allocate(b_size);
    void* c = ThreadHeap::allocate(c_size);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    ASSERT_NE(c, nullptr);

    EXPECT_EQ(ChunkBase(a), ChunkBase(b));
    EXPECT_EQ(ChunkBase(b), ChunkBase(c));
    std::memset(a, 0x11, a_size);
    std::memset(b, 0x22, b_size);
    std::memset(c, 0x33, c_size);

    ThreadHeap::deallocate(b);
    void* reused = ThreadHeap::allocate(b_size);
    ASSERT_NE(reused, nullptr);
    EXPECT_EQ(reused, b);
    EXPECT_EQ(static_cast<unsigned char*>(a)[a_size - 1], 0x11);
    EXPECT_EQ(static_cast<unsigned char*>(c)[c_size - 1], 0x33);

    ThreadHeap::deallocate(reused);
    ThreadHeap::deallocate(a);
    ThreadHeap::deallocate(c);
}

TEST(MediumArenaTest, OwnerThreadCanExitBeforeCrossThreadFree) {
    constexpr std::size_t size = 512 * 1024;
    std::promise<void*> promise;
    auto result = promise.get_future();

    std::thread producer([&promise] {
        void* ptr = ThreadHeap::allocate(size);
        if (ptr) {
            std::memset(ptr, 0x5A, size);
        }
        promise.set_value(ptr);
    });

    void* ptr = result.get();
    producer.join();

    ASSERT_NE(ptr, nullptr);
    EXPECT_EQ(ChunkHeader::Get(ptr)->type, ChunkHeader::Type::MEDIUM);
    EXPECT_EQ(static_cast<unsigned char*>(ptr)[size - 1], 0x5A);
    ThreadHeap::deallocate(ptr);
}

TEST(MediumArenaTest, ConcurrentAllocateAndFree) {
    constexpr int threads = 6;
    constexpr int iterations = 80;
    std::atomic<bool> valid{true};
    std::vector<std::thread> workers;

    for (int t = 0; t < threads; ++t) {
        workers.emplace_back([t, &valid] {
            for (int i = 0; i < iterations; ++i) {
                const std::size_t size =
                    SizeClassConfig::kMaxAlloc + 1 +
                    static_cast<std::size_t>(i % 5) * 4096;
                void* ptr = ThreadHeap::allocate(size);
                if (!ptr) {
                    valid.store(false, std::memory_order_relaxed);
                    return;
                }
                auto* bytes = static_cast<unsigned char*>(ptr);
                bytes[0] = static_cast<unsigned char>(t + 1);
                bytes[size - 1] = static_cast<unsigned char>(i);
                if (bytes[0] != static_cast<unsigned char>(t + 1) ||
                    bytes[size - 1] != static_cast<unsigned char>(i)) {
                    valid.store(false, std::memory_order_relaxed);
                }
                ThreadHeap::deallocate(ptr);
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    EXPECT_TRUE(valid.load(std::memory_order_relaxed));
}

TEST(MediumArenaTest, HugeAllocationUsesCorrectlyRoundedMapping) {
    constexpr std::size_t size = 3 * 1024 * 1024 + 17;
    void* ptr = ThreadHeap::allocate(size);
    ASSERT_NE(ptr, nullptr);
    EXPECT_EQ(ChunkHeader::Get(ptr)->type, ChunkHeader::Type::LARGE);
    auto* bytes = static_cast<unsigned char*>(ptr);
    bytes[0] = 0xA5;
    bytes[size - 1] = 0x5A;
    EXPECT_EQ(bytes[0], 0xA5);
    EXPECT_EQ(bytes[size - 1], 0x5A);
    ThreadHeap::deallocate(ptr);

    EXPECT_EQ(ThreadHeap::allocate(std::numeric_limits<std::size_t>::max()),
              nullptr);
}
