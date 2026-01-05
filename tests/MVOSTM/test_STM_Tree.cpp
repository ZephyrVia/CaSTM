#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <algorithm>
#include <random>
#include <chrono>
#include <memory>
#include <iostream>

#include "MVOSTM/STM.hpp"

// 可选：如果以后修复了 ThreadHeap，可取消注释
// #include "ThreadHeap/ThreadHeap.hpp" 

// ==========================================
// 1. 定义树节点
// ==========================================
struct TreeNode {
    int key;
    STM::Var<TreeNode*> left;
    STM::Var<TreeNode*> right;

    TreeNode(int k) : key(k), left(nullptr), right(nullptr) {}
    
    // 暂时禁用自定义分配器
};

// ==========================================
// 2. 树操作辅助类
// ==========================================
class BST {
public:
    STM::Var<TreeNode*> root;

    BST() : root(nullptr) {}

    // 递归插入
    void insert(Transaction& tx, STM::Var<TreeNode*>& currentVar, int key) {
        TreeNode* curr = tx.load(currentVar);

        if (curr == nullptr) {
            auto newNode = std::unique_ptr<TreeNode>(new TreeNode(key));
            tx.store(currentVar, newNode.release());
            return;
        }

        if (key == curr->key) {
            return;
        } else if (key < curr->key) {
            insert(tx, curr->left, key);
        } else {
            insert(tx, curr->right, key);
        }
    }

    // 中序遍历
    void inorder(Transaction& tx, STM::Var<TreeNode*>& currentVar, std::vector<int>& result) {
        TreeNode* curr = tx.load(currentVar);
        if (curr == nullptr) return;

        inorder(tx, curr->left, result);
        result.push_back(curr->key);
        inorder(tx, curr->right, result);
    }

    // 【核心修复】：改为“收集”而不是“删除”
    // 我们只负责把节点指针收集起来，并不立即 delete，防止破坏事务的 ReadSet
    void collect_garbage(Transaction& tx, STM::Var<TreeNode*>& currentVar, std::vector<TreeNode*>& out) {
        TreeNode* curr = tx.load(currentVar);
        if (curr == nullptr) return;

        collect_garbage(tx, curr->left, out);
        collect_garbage(tx, curr->right, out);

        out.push_back(curr);
        
        // 逻辑上断开连接
        tx.store<TreeNode*>(currentVar, nullptr);
    }
};

class STMTreeTest : public ::testing::Test {};

// ==========================================
// 3. 基础串行测试
// ==========================================
TEST_F(STMTreeTest, SequentialInsert) {
    BST tree;
    STM::atomically([&](Transaction& tx) {
        tree.insert(tx, tree.root, 50);
        tree.insert(tx, tree.root, 20);
        tree.insert(tx, tree.root, 70);
    });

    // 显式清理步骤：
    std::vector<TreeNode*> garbage;
    STM::atomically([&](Transaction& tx) {
        garbage.clear(); // 必须清空，防止事务重试导致重复添加
        tree.collect_garbage(tx, tree.root, garbage);
    });

    // 事务提交后，安全删除
    for (auto* node : garbage) {
        delete node;
    }
}

// ==========================================
// 4. 并发压力测试
// ==========================================
TEST_F(STMTreeTest, ConcurrentInsertStress) {
    BST tree;
    const int NUM_THREADS = 2; 
    const int ITEMS_PER_THREAD = 20; 
    const int TOTAL_ITEMS = NUM_THREADS * ITEMS_PER_THREAD;

    std::vector<int> all_keys;
    all_keys.reserve(TOTAL_ITEMS);
    for (int i = 0; i < TOTAL_ITEMS; ++i) all_keys.push_back(i);

    std::random_device rd;
    std::mt19937 g(rd());
    std::shuffle(all_keys.begin(), all_keys.end(), g);

    std::cout << "[INFO] Starting Insertion with " << NUM_THREADS << " threads..." << std::endl;

    std::vector<std::thread> workers;
    for (int i = 0; i < NUM_THREADS; ++i) {
        workers.emplace_back([&, i]() {
            int start = i * ITEMS_PER_THREAD;
            int end = start + ITEMS_PER_THREAD;
            for (int k = start; k < end; ++k) {
                STM::atomically([&](Transaction& tx) {
                    tree.insert(tx, tree.root, all_keys[k]);
                });
            }
        });
    }

    for (auto& t : workers) t.join();
    std::cout << "[INFO] Insertion Finished." << std::endl;

    // 验证
    std::vector<int> result;
    STM::atomically([&](Transaction& tx) {
        tree.inorder(tx, tree.root, result);
    });
    EXPECT_EQ(result.size(), TOTAL_ITEMS);
    EXPECT_TRUE(std::is_sorted(result.begin(), result.end()));

    // 【核心修复】：两阶段清理
    std::vector<TreeNode*> garbage;
    STM::atomically([&](Transaction& tx) {
        garbage.clear(); // 应对 Retry
        tree.collect_garbage(tx, tree.root, garbage);
    });
    for (auto* node : garbage) delete node;
}

// ==========================================
// 5. 读写隔离测试
// ==========================================
TEST_F(STMTreeTest, ReaderWriterIsolation) {
    BST tree;
    std::atomic<bool> done{false};
    const int TOTAL_ITEMS = 200;

    std::thread writer([&]() {
        for (int i = 0; i < TOTAL_ITEMS; ++i) {
            STM::atomically([&](Transaction& tx) {
                tree.insert(tx, tree.root, i * 2);
            });
            if (i % 20 == 0) std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        done = true;
    });

    std::thread reader([&]() {
        while (!done) {
            std::vector<int> snapshot;
            try {
                STM::atomically([&](Transaction& tx) {
                    snapshot.clear();
                    tree.inorder(tx, tree.root, snapshot);
                });
                if (!snapshot.empty()) EXPECT_TRUE(std::is_sorted(snapshot.begin(), snapshot.end()));
            } catch (...) {}
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    writer.join();
    reader.join();

    // 两阶段清理
    std::vector<TreeNode*> garbage;
    STM::atomically([&](Transaction& tx) {
        garbage.clear();
        tree.collect_garbage(tx, tree.root, garbage);
    });
    for (auto* node : garbage) delete node;
}