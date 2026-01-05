#include <gtest/gtest.h>
#include <vector>
#include <cstring>

// 包含你的头文件
#include "ThreadHeap/Slab.hpp"
#include "common/GlobalConfig.hpp"

// 前置声明模拟
class SizeClassPool {}; 

class ChunkMetadataTest : public ::testing::Test {
protected:
    void SetUp() override {
        // 1. 模拟申请一个 2MB 的 Chunk (必须对齐)
        // 使用 aligned_alloc (C++17) 或 posix_memalign
        // 这里为了跨平台简单模拟，分配稍大一点然后手动对齐
        raw_memory_ = new char[kChunkSize + kChunkAlignment];
        
        uintptr_t raw_addr = reinterpret_cast<uintptr_t>(raw_memory_);
        aligned_chunk_start_ = reinterpret_cast<void*>(
            (raw_addr + kChunkAlignment - 1) & ~(kChunkAlignment - 1)
        );

        // 2. 模拟一个 Pool 指针
        dummy_pool_ = reinterpret_cast<SizeClassPool*>(0xDEADBEEF);
    }

    void TearDown() override {
        delete[] raw_memory_;
    }

    // 辅助：检查指针是否在 Bump Pointer 区域内
    bool IsInBumpArea(void* ptr, Slab* meta) {
        // 这是一个黑盒测试，我们只能通过分配行为推断，
        // 或者通过友元/Getter访问私有变量。
        // 这里我们通过地址范围判断。
        return ptr > (void*)meta && ptr < (void*)((char*)aligned_chunk_start_ + kChunkSize);
    }

    char* raw_memory_ = nullptr;
    void* aligned_chunk_start_ = nullptr;
    SizeClassPool* dummy_pool_ = nullptr;
};

// 1. 初始化测试
TEST_F(ChunkMetadataTest, Initialization) {
    uint32_t block_size = 64;
    auto* meta = Slab::CreateAt(aligned_chunk_start_, dummy_pool_, block_size);

    ASSERT_NE(meta, nullptr);
    EXPECT_EQ(meta->owner(), dummy_pool_);
    EXPECT_EQ(meta->block_size(), block_size);
    EXPECT_EQ(meta->allocated_count(), 0);
    EXPECT_TRUE(meta->isEmpty());
    EXPECT_FALSE(meta->isFull());

    // 验证最大块数计算是否合理 (2MB - Header) / 64
    // Header 至少是 sizeof(ChunkMetadata) 对齐到 64
    size_t header_size = (sizeof(Slab) + 63) & ~63;
    uint32_t expected_max = (kChunkSize - header_size) / block_size;
    EXPECT_EQ(meta->max_block_count(), expected_max);
}

// 2. Bump Pointer 分配测试
TEST_F(ChunkMetadataTest, AllocatesSequentiallyUsingBumpPointer) {
    uint32_t block_size = 128;
    auto* meta = Slab::CreateAt(aligned_chunk_start_, dummy_pool_, block_size);

    void* p1 = meta->allocate();
    void* p2 = meta->allocate();
    void* p3 = meta->allocate();

    ASSERT_NE(p1, nullptr);
    ASSERT_NE(p2, nullptr);
    ASSERT_NE(p3, nullptr);

    // 验证地址连续性 (Bump Pointer 行为)
    EXPECT_EQ(static_cast<char*>(p2), static_cast<char*>(p1) + block_size);
    EXPECT_EQ(static_cast<char*>(p3), static_cast<char*>(p2) + block_size);

    EXPECT_EQ(meta->allocated_count(), 3);
}

// 3. Local Free & Reuse (LIFO) 测试
TEST_F(ChunkMetadataTest, LocalFreeAndReuseLIFO) {
    auto* meta = Slab::CreateAt(aligned_chunk_start_, dummy_pool_, 64);

    void* p1 = meta->allocate();
    void* p2 = meta->allocate();
    void* p3 = meta->allocate();

    // 此时 Allocated: 3
    EXPECT_EQ(meta->allocated_count(), 3);

    // 归还 p2 (中间那个)
    bool isEmpty = meta->freeLocal(p2);
    EXPECT_FALSE(isEmpty);
    EXPECT_EQ(meta->allocated_count(), 2);

    // 再次申请，应该优先复用 p2 (LIFO)
    void* p4 = meta->allocate();
    EXPECT_EQ(p4, p2) << "Should reuse the most recently freed local block";
    EXPECT_EQ(meta->allocated_count(), 3);

    // 再次申请，应该继续走 Bump Pointer (得到 p3 后面的新地址)
    void* p5 = meta->allocate();
    EXPECT_EQ(static_cast<char*>(p5), static_cast<char*>(p3) + 64);
}

// 4. Remote Free & Reclaim 测试 (核心逻辑)
TEST_F(ChunkMetadataTest, RemoteFreeAndReclaim) {
    auto* meta = Slab::CreateAt(aligned_chunk_start_, dummy_pool_, 64);

    void* p1 = meta->allocate();
    void* p2 = meta->allocate();
    
    EXPECT_EQ(meta->allocated_count(), 2);

    // 模拟远程释放 p1
    meta->freeRemote(p1);

    // 关键点：远程释放不减少 Allocated Count
    EXPECT_EQ(meta->allocated_count(), 2);

    // 手动触发回收
    uint32_t reclaimed = meta->reclaimRemoteMemory();
    
    // 验证回收结果
    EXPECT_EQ(reclaimed, 1);
    EXPECT_EQ(meta->allocated_count(), 1); // 2 - 1 = 1

    // 再次分配，应该拿到刚才回收的 p1
    void* p3 = meta->allocate();
    EXPECT_EQ(p3, p1);
    EXPECT_EQ(meta->allocated_count(), 2);
}

// 5. 自动触发回收测试 (allocate 内部逻辑)
TEST_F(ChunkMetadataTest, AllocateTriggersReclaimAutomatically) {
    // 为了方便测试 Bump Pointer 耗尽，我们设置一个极大的 block_size
    // 或者我们手动填满。这里选择手动填满小 Chunk 比较困难，
    // 我们信任逻辑：bump_ptr 满了 -> local_list 空了 -> 触发 reclaim
    
    // 技巧：我们不需要真的把 bump pointer 耗尽，
    // 只要 local_list 为空，且 bump pointer 也没满，
    // reclaim_remote_memory() 被调用是无害的（返回0）。
    // 但为了测试 allocate 里的递归逻辑，我们需要构造：
    // 1. local_list 空
    // 2. bump pointer 满 (模拟) -> 这里比较难 mock 私有变量
    // 
    // 替代方案：黑盒测试。只要 Remote 有东西，allocate 就应该能拿回来。
    // 虽然如果 Bump 没满，它会优先切新肉。
    // 所以这个测试只能验证：当分配了大量内存后，远程归还的内存最终能被复用。
    
    // 构造场景：
    // 1. 创建 Chunk
    auto* meta = Slab::CreateAt(aligned_chunk_start_, dummy_pool_, 64);
    
    // 2. 将 Chunk 填满 (或者填很多)
    std::vector<void*> ptrs;
    uint32_t max = meta->max_block_count();
    // 填满整个 Chunk
    for(uint32_t i=0; i < max; ++i) {
        void* p = meta->allocate();
        ASSERT_NE(p, nullptr);
        ptrs.push_back(p);
    }
    
    EXPECT_TRUE(meta->isFull());
    EXPECT_EQ(meta->allocate(), nullptr); // 应该 OOM

    // 3. 远程释放一半
    size_t half = max / 2;
    for(size_t i=0; i < half; ++i) {
        meta->freeRemote(ptrs[i]);
    }

    // 此时 allocated_count 依然是满的
    EXPECT_TRUE(meta->isFull());

    // 4. 再次 allocate
    // 此时 local_free_list 为空，bump_ptr 已满。
    // 唯一的出路是 reclaim_remote_memory() -> 递归 allocate()
    void* reused_ptr = meta->allocate();
    
    ASSERT_NE(reused_ptr, nullptr) << "Should reclaim remote memory when chunk is full";
    EXPECT_FALSE(meta->isFull()); // 回收后 count 修正，肯定不满了
    
    // 验证拿到的确实是刚才释放的某一个
    bool found = false;
    for(size_t i=0; i < half; ++i) {
        if (ptrs[i] == reused_ptr) {
            found = true; 
            break;
        }
    }
    EXPECT_TRUE(found);
}

// 6. 状态流转测试 (Empty -> Full -> Empty)
TEST_F(ChunkMetadataTest, StateTransitions) {
    // 使用巨大的块大小，使得 max_count 很小，方便测试 Full
    // 2MB / 1MB = ~1 个块 (除去 header 可能存不下 2 个)
    // 让我们算一下：Header ~ 128B. 2MB - 128B. 
    // 设 BlockSize = 1MB (1024*1024). Max = 1.
    uint32_t huge_block = 1024 * 1024; 
    auto* meta = Slab::CreateAt(aligned_chunk_start_, dummy_pool_, huge_block);

    EXPECT_TRUE(meta->isEmpty());

    void* p1 = meta->allocate();
    ASSERT_NE(p1, nullptr);
    EXPECT_FALSE(meta->isEmpty());
    
    // 如果只能存下一个，现在应该是 Full
    if (meta->max_block_count() == 1) {
        EXPECT_TRUE(meta->isFull());
        EXPECT_EQ(meta->allocate(), nullptr);
    }

    // 本地释放
    bool becomeEmpty = meta->freeLocal(p1);
    EXPECT_TRUE(becomeEmpty);
    EXPECT_TRUE(meta->isEmpty());
    EXPECT_FALSE(meta->isFull());
}