#include <gtest/gtest.h>
#include "common/SizeClassConfig.hpp"


class SizeClassConfigTest : public ::testing::Test {
protected:
    // 在所有测试开始前初始化一次配置表
    static void SetUpTestSuite() {
        SizeClassConfig::Init();
    }
};

// 1. 测试微型对象 (Tiny Objects, 8B - 128B) 的快速路径
// 这一段逻辑对应代码中的位运算优化
TEST_F(SizeClassConfigTest, TinyObjectMapping) {
    // 0-8 bytes -> Index 0 (Size 8)
    EXPECT_EQ(SizeClassConfig::SizeToClass(1), 0);
    EXPECT_EQ(SizeClassConfig::SizeToClass(8), 0);
    EXPECT_EQ(SizeClassConfig::ClassToSize(0), 8);

    // 9-16 bytes -> Index 1 (Size 16)
    EXPECT_EQ(SizeClassConfig::SizeToClass(9), 1);
    EXPECT_EQ(SizeClassConfig::SizeToClass(16), 1);
    EXPECT_EQ(SizeClassConfig::ClassToSize(1), 16);

    // 120 bytes -> Index 14
    EXPECT_EQ(SizeClassConfig::ClassToSize(14), 120);

    // 121-128 bytes -> Index 15 (Size 128) - 快速路径的终点
    EXPECT_EQ(SizeClassConfig::SizeToClass(121), 15);
    EXPECT_EQ(SizeClassConfig::SizeToClass(128), 15);
    EXPECT_EQ(SizeClassConfig::ClassToSize(15), 128);
}

// 2. 测试步长切换点 (Transition Points)
// 确保当大小超过某个区间时，能够正确跳到下一个步长的起始值
TEST_F(SizeClassConfigTest, StepTransitions) {
    // --- Transition 1: Step 8 -> Step 16 ---
    // 128B (Index 15) -> 下一个应该是 144B (Index 16)
    // 如果申请 129B，应该分配 144B
    size_t idx_129 = SizeClassConfig::SizeToClass(129);
    EXPECT_EQ(idx_129, 16); 
    EXPECT_EQ(SizeClassConfig::ClassToSize(16), 144);

    // --- Transition 2: Step 16 -> Step 32 ---
    // 256B 是 Step 16 的终点。
    // 257B 应该跳到 288B
    size_t idx_257 = SizeClassConfig::SizeToClass(257);
    size_t size_257 = SizeClassConfig::ClassToSize(idx_257);
    EXPECT_EQ(size_257, 288);
    
    // --- Transition 3: Step 32 -> Step 64 ---
    // 512B 是 Step 32 的终点
    // 513B 应该跳到 576B
    EXPECT_EQ(SizeClassConfig::Normalize(513), 576);
}

// 3. 测试随机采样的一致性 (Consistency)
// 验证 SizeToClass 和 ClassToSize 的闭环逻辑
TEST_F(SizeClassConfigTest, RoundTripConsistency) {
    // 测试几个关键值
    size_t test_sizes[] = { 32, 42, 100, 250, 1000, 4096, 20000, 260000 };
    
    for (size_t request : test_sizes) {
        if (request > SizeClassConfig::kMaxAlloc) continue;

        size_t idx = SizeClassConfig::SizeToClass(request);
        size_t allocated = SizeClassConfig::ClassToSize(idx);

        // 分配的大小必须 >= 请求的大小
        EXPECT_GE(allocated, request);
        
        // 验证没有分配过大的内存（检查是否跳过了某个规格）
        // 比如请求 33，应该给 40，而不是 48
        if (idx > 0) {
            size_t prev_size = SizeClassConfig::ClassToSize(idx - 1);
            EXPECT_LT(prev_size, request) << "Index selected is too large, previous class could have satisfied request";
        }
    }
}

// 4. 测试 Normalize 函数
TEST_F(SizeClassConfigTest, NormalizeLogic) {
    // 正常范围内
    EXPECT_EQ(SizeClassConfig::Normalize(7), 8);
    EXPECT_EQ(SizeClassConfig::Normalize(128), 128);
    EXPECT_EQ(SizeClassConfig::Normalize(129), 144);

    // 大内存范围 (> 256KB)
    // 应该按页 (4KB) 对齐
    size_t large_req = 256 * 1024 + 1; // 256KB + 1B
    size_t expected = 256 * 1024 + 4096; // 应该是 260KB (如果是按4KB向上取整的话)
    
    // 手动计算 RoundUp(262145, 4096)
    // 262144 是 4KB 的倍数 (64 pages)
    // 262145 应该 round up 到 266240
    EXPECT_EQ(SizeClassConfig::Normalize(large_req), expected);
}

// 5. 验证最大规格
TEST_F(SizeClassConfigTest, MaxBoundaries) {
    size_t max_small = 256 * 1024;
    size_t idx = SizeClassConfig::SizeToClass(max_small);
    
    // 确保没有越界
    EXPECT_LT(idx, SizeClassConfig::kClassCount);
    EXPECT_EQ(SizeClassConfig::ClassToSize(idx), max_small);
    
    // 确保超出一点点就会越界返回 kClassCount
    EXPECT_EQ(SizeClassConfig::SizeToClass(max_small + 1), SizeClassConfig::kClassCount);
}