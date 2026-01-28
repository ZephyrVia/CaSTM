#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <algorithm> // std::shuffle
#include <random>    // std::mt19937

// 引入您的头文件
#include "WwSTM/TxContext.hpp"
#include "TierAlloc/ThreadHeap/ThreadHeap.hpp"

using namespace STM::Ww;

// ==========================================
// 1. 数据结构 (指针聚合模式)
// ==========================================
struct TreeNode {
    int val;
    // 使用 TMVar* 指针，保护子节点指针
    TMVar<TreeNode*>* left;
    TMVar<TreeNode*>* right;

    TreeNode(int v) : val(v), left(nullptr), right(nullptr) {}
};

// ==========================================
// 2. 辅助函数
// ==========================================

// 工厂函数：分配节点本体 + 两个 STM 变量插槽
TreeNode* alloc_tree_node(TxContext& tx, int val) {
    void* node_mem = ThreadHeap::allocate(sizeof(TreeNode));
    TreeNode* node = new (node_mem) TreeNode(val);

    // 为左右指针分配 STM 保护壳 (初始值为 nullptr)
    node->left = tx.alloc<TreeNode*>(nullptr);
    node->right = tx.alloc<TreeNode*>(nullptr);

    return node;
}

// STM 重试循环
template<typename F>
void atomically(F&& func) {
    TxContext tx; 
    while (true) {
        func(tx);
        if (tx.commit()) break; 
        tx.begin(); 
    }
}

// 中序遍历 (验证用)
void inorder_traversal(TxContext& tx, TreeNode* node, std::vector<int>& result) {
    if (!node) return;
    
    // 指针 API：直接传 node->left
    TreeNode* left_child = tx.read(node->left);
    inorder_traversal(tx, left_child, result);
    
    result.push_back(node->val);
    
    TreeNode* right_child = tx.read(node->right);
    inorder_traversal(tx, right_child, result);
}

// 收集节点 (清理用)
void collect_nodes(TxContext& tx, TreeNode* node, std::vector<TreeNode*>& out) {
    if (!node) return;
    
    TreeNode* l = tx.read(node->left);
    TreeNode* r = tx.read(node->right);
    
    collect_nodes(tx, l, out);
    collect_nodes(tx, r, out);
    
    out.push_back(node);
}

// ==========================================
// 3. 测试夹具
// ==========================================
class WwSTMTest : public ::testing::Test {
protected:
    // 全局 Root 变量
    TMVar<TreeNode*>* root_var;

    void SetUp() override {
        // 在系统堆上初始化 Root，payload 为空
        root_var = new TMVar<TreeNode*>(nullptr);
    }

    void TearDown() override {
        delete root_var;
    }
};

// ==========================================
// 4. 并发测试逻辑 (修复了死循环/栈溢出问题)
// ==========================================

TEST_F(WwSTMTest, ConcurrentBST_Randomized_Input) {
    const int NUM_THREADS = 8;
    const int ITEMS_PER_THREAD = 500; // 总共 4000 个节点
    const int TOTAL_ITEMS = NUM_THREADS * ITEMS_PER_THREAD;

    // 【关键修复 1】生成所有 Key 并打乱顺序
    // 避免有序插入导致 BST 退化成链表 (Stack Overflow 的元凶)
    std::vector<int> all_keys;
    all_keys.reserve(TOTAL_ITEMS);
    for (int i = 0; i < TOTAL_ITEMS; ++i) {
        all_keys.push_back(i);
    }

    // 使用随机种子打乱
    std::random_device rd;
    std::mt19937 g(rd());
    std::shuffle(all_keys.begin(), all_keys.end(), g);

    std::vector<std::thread> workers;

    // 分发任务：每个线程处理 all_keys 的一部分
    for (int i = 0; i < NUM_THREADS; ++i) {
        workers.emplace_back([&, i]() {
            int start = i * ITEMS_PER_THREAD;
            int end = start + ITEMS_PER_THREAD;

            for (int k = start; k < end; ++k) {
                int val_to_insert = all_keys[k];

                atomically([&](TxContext& tx) {
                    // 1. 分配新节点
                    TreeNode* new_node = alloc_tree_node(tx, val_to_insert);
                    
                    // 2. 读取 Root
                    TreeNode* curr = tx.read(root_var);
                    
                    if (curr == nullptr) {
                        tx.write(root_var, new_node);
                        return;
                    }

                    // 3. 查找插入位置
                    while (true) {
                        if (val_to_insert < curr->val) {
                            // 向左：直接用 read(ptr)
                            TreeNode* left = tx.read(curr->left);
                            if (left == nullptr) {
                                tx.write(curr->left, new_node);
                                break;
                            }
                            curr = left;
                        } else {
                            // 向右：直接用 read(ptr)
                            TreeNode* right = tx.read(curr->right);
                            if (right == nullptr) {
                                tx.write(curr->right, new_node);
                                break;
                            }
                            curr = right;
                        }
                    }
                });
            }
        });
    }

    // 等待所有线程完成
    for (auto& t : workers) {
        t.join();
    }

    // ==========================================
    // 验证阶段
    // ==========================================
    atomically([&](TxContext& tx) {
        std::vector<int> sorted_vals;
        TreeNode* root_node = tx.read(root_var);
        
        // 这里的递归现在是安全的，因为树高度约为 log2(4000) ≈ 12-20 层
        inorder_traversal(tx, root_node, sorted_vals);

        // 1. 验证数量
        EXPECT_EQ(sorted_vals.size(), TOTAL_ITEMS)
            << "Tree size mismatch! Lost updates detected.";

        // 2. 验证有序性
        bool is_sorted = std::is_sorted(sorted_vals.begin(), sorted_vals.end());
        EXPECT_TRUE(is_sorted) << "BST property violated! Result is not sorted.";
        
        // 3. 验证无重复
        auto last = std::unique(sorted_vals.begin(), sorted_vals.end());
        EXPECT_EQ(last, sorted_vals.end()) << "Duplicate values found!";
    });

    // ==========================================
    // 清理阶段
    // ==========================================
    std::vector<TreeNode*> nodes_to_delete;
    
    // 1. 收集所有指针
    atomically([&](TxContext& tx) {
        nodes_to_delete.clear();
        TreeNode* root_node = tx.read(root_var);
        collect_nodes(tx, root_node, nodes_to_delete);
        
        // 断开 Root，防止后续访问
        tx.write(root_var, (TreeNode*)nullptr);
    });

    // 2. 释放内存
    // 注意：这里的释放依赖于您是否在 STM 中实现了安全的内存回收 (EBR)
    // 如果没有实现 delete 逻辑，可以注释掉下面这块，防止 Double Free
    /*
    atomically([&](TxContext& tx) {
         // 这里如果需要显式释放，需要调用您特定的释放接口
         // 且需要注意释放顺序 (先释放子 TMVar 还是先释放 TreeNode)
         // 由于 TreeNode 内部的 left/right 也是 alloc 出来的，
         // 这是一个比较复杂的递归释放过程。
         // 只要上面断开 Root 且通过了内存检查工具，测试本身可以到此结束。
    });
    */
}