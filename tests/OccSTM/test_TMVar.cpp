#include <gtest/gtest.h>
#include <string>

// 包含你的核心头文件
#include "OccSTM/TMVar.hpp"
// VersionNode.hpp 通常被 TMVar 包含，但为了测试 detail 里的结构，也可以显式包含
#include "OccSTM/VersionNode.hpp"

using namespace STM::Occ;

namespace {

// 一个用于测试的自定义结构体
struct ComplexData {
    int id;
    std::string name;
    
    // 必须提供默认构造函数，因为 TMVar 的 Genesis Node 默认构造 T{}
    ComplexData() : id(0), name("default") {}
    ComplexData(int i, std::string n) : id(i), name(n) {}
    
    bool operator==(const ComplexData& other) const {
        return id == other.id && name == other.name;
    }
};

}

class OccDataStructureTest : public ::testing::Test {
protected:
    void SetUp() override {
        // 如果 ThreadHeap 需要初始化，在这里调用
        // TierAlloc::ThreadHeap::init(); 
    }
    
    void TearDown() override {
    }
};

// 测试 1: 基础整数类型的初始化 (Genesis Node)
TEST_F(OccDataStructureTest, IntegerInitialization) {
    // 实例化 TMVar<int>
    // 内部调用 new Node(0, nullptr, int{})
    TMVar<int> var;

    // 验证 head 指针不为空
    // TMVar<T>::Node 是 detail::VersionNode<T> 的别名
    auto* head = var.loadHead();
    ASSERT_NE(head, nullptr) << "TMVar head should not be null after construction";

    // 验证创世版本信息
    EXPECT_EQ(head->write_ts, 0) << "Genesis version should be 0";
    EXPECT_EQ(head->prev, nullptr) << "Genesis node should have no previous version";
    EXPECT_EQ(head->payload, 0) << "Payload should be default constructed (0 for int)";
}

// 测试 2: 复杂对象的初始化
TEST_F(OccDataStructureTest, ComplexObjectInitialization) {
    // 显式调用有参构造函数
    TMVar<ComplexData> var(10, "init");
    
    auto* head = var.loadHead();
    ASSERT_NE(head, nullptr);
    
    // 验证 VersionNode 是否正确转发了参数给 ComplexData
    EXPECT_EQ(head->payload.id, 10);
    EXPECT_EQ(head->payload.name, "init");
    EXPECT_EQ(head->write_ts, 0);
}

// 测试 3: 模拟事务更新 (链表挂载)
TEST_F(OccDataStructureTest, VersionChaining) {
    TMVar<int> var(0); // head -> Node(ts=0, payload=0)
    
    // 1. 获取当前头节点
    auto* old_head = var.loadHead();
    
    // 2. 模拟事务：创建新节点 (Version 100, Payload 42)
    // 使用 TMVar<int>::Node 别名访问 detail::VersionNode<int>
    using Node = TMVar<int>::Node;
    
    // 构造函数签名: (wts, prev, args...)
    Node* new_node = new Node(100, old_head, 42);
    
    // 3. 模拟 CAS 更新 head
    // 实际事务中会锁住 StripedLock，这里直接 atomic store 测试数据结构行为
    var.getHeadRef().store(new_node);
    
    // 4. 验证链表结构
    auto* current_head = var.loadHead();
    EXPECT_EQ(current_head, new_node);
    EXPECT_EQ(current_head->payload, 42);
    EXPECT_EQ(current_head->write_ts, 100);
    
    // 验证回溯 (History Traversal)
    ASSERT_NE(current_head->prev, nullptr);
    EXPECT_EQ(current_head->prev, old_head);
    EXPECT_EQ(current_head->prev->payload, 0); 
    EXPECT_EQ(current_head->prev->write_ts, 0);
}

// 测试 4: 验证 ThreadHeap/Operator New 是否正常工作
TEST_F(OccDataStructureTest, AllocationSanity) {
    TMVar<int> var;
    auto* n1 = var.loadHead();
    
    using Node = TMVar<int>::Node;
    Node* n2 = new Node(1, n1, 123);
    
    // 简单的检查：地址不应为空，且不应相同
    EXPECT_NE(n1, nullptr);
    EXPECT_NE(n2, nullptr);
    EXPECT_NE(n1, n2);

    // 清理手动分配的节点 (TMVar 析构只清理 head 链)
    // 注意：如果有 ThreadHeap，这里 operator delete 会调用 ThreadHeap::deallocate
    delete n2; 
}