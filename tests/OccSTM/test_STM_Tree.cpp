#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <algorithm>
#include <random>
#include <chrono>
#include <iostream>

// 确保包含你的 STM 头文件
#include "OccSTM/STM.hpp"

using namespace STM::Occ;

// ==========================================
// 1. 定义树节点
// ==========================================
struct TreeNode {
    int key;
    STM::Var<TreeNode*> left;
    STM::Var<TreeNode*> right;

    TreeNode(int k) : key(k), left(nullptr), right(nullptr) {}
};

// ==========================================
// 2. 树操作辅助类 (BST)
// ==========================================
class BST {
public:
    STM::Var<TreeNode*> root;

    BST() : root(nullptr) {}

    // 递归插入
    // 注意：这里的 Transaction 现在是 STM::Occ::Transaction
    void insert(Transaction& tx, STM::Var<TreeNode*>& currentVar, int key) {
        TreeNode* curr = tx.load(currentVar);

        // Case 1: 插入位置为空
        if (curr == nullptr) {
            // 使用 tx.alloc 替代 new，由 STM 管理生命周期
            TreeNode* newNode = tx.alloc<TreeNode>(key);
            tx.store(currentVar, newNode);
            return;
        }

        // Case 2: 键值已存在
        if (key == curr->key) {
            return;
        } 
        // Case 3: 递归向下
        else if (key < curr->key) {
            insert(tx, curr->left, key);
        } else {
            insert(tx, curr->right, key);
        }
    }

    // 中序遍历 (验证有序性)
    void inorder(Transaction& tx, STM::Var<TreeNode*>& currentVar, std::vector<int>& result) {
        TreeNode* curr = tx.load(currentVar);
        if (curr == nullptr) return;

        inorder(tx, curr->left, result);
        result.push_back(curr->key); // 访问根
        inorder(tx, curr->right, result);
    }

    // 安全的垃圾回收收集器
    void collect_garbage(Transaction& tx, STM::Var<TreeNode*>& currentVar, std::vector<TreeNode*>& out) {
        TreeNode* curr = tx.load(currentVar);
        if (curr == nullptr) return;

        // 递归收集子节点
        collect_garbage(tx, curr->left, out);
        collect_garbage(tx, curr->right, out);

        // 收集当前节点
        out.push_back(curr);
        
        // 从 STM 视角断开连接（置空）
        tx.store<TreeNode*>(currentVar, nullptr);
    }
};

class STMTreeTest : public ::testing::Test {};

// ==========================================
// 测试用例 1: 中等压力的并发插入
// ==========================================
TEST_F(STMTreeTest, ConcurrentInsert_MediumStress) {
    BST tree;
    
    // 配置：中等压力
    const int NUM_THREADS = 4; 
    const int ITEMS_PER_THREAD = 100; 
    const int TOTAL_ITEMS = NUM_THREADS * ITEMS_PER_THREAD;

    // 1. 准备数据
    std::vector<int> all_keys;
    all_keys.reserve(TOTAL_ITEMS);
    for (int i = 0; i < TOTAL_ITEMS; ++i) all_keys.push_back(i);

    // 2. 随机打乱
    std::random_device rd;
    std::mt19937 g(rd());
    std::shuffle(all_keys.begin(), all_keys.end(), g);

    std::cout << "[INFO] Starting Medium Stress Test (" << NUM_THREADS << " threads, " << TOTAL_ITEMS << " items)..." << std::endl;

    std::vector<std::thread> workers;
    
    // 3. 启动线程并发插入
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

    // 4. 等待结束
    for (auto& t : workers) {
        t.join();
    }
    std::cout << "[INFO] Insertion Finished." << std::endl;

    // 5. 验证结果
    std::vector<int> result;
    STM::atomically([&](Transaction& tx) {
        tree.inorder(tx, tree.root, result);
    });

    // 验证大小
    EXPECT_EQ(result.size(), TOTAL_ITEMS) << "Tree size mismatch! Likely Lost Update.";
    
    // 验证有序性
    EXPECT_TRUE(std::is_sorted(result.begin(), result.end())) << "Tree structure corrupted (not sorted).";

    // 6. 清理内存
    std::vector<TreeNode*> garbage;
    STM::atomically([&](Transaction& tx) {
        garbage.clear(); 
        tree.collect_garbage(tx, tree.root, garbage);
    });
    
    STM::atomically([&](Transaction& tx) {
        for (auto* node : garbage) {
            tx.free(node);
        }
    });
}

// ==========================================
// 测试用例 2: 读写隔离 (Snapshot Isolation)
// ==========================================
TEST_F(STMTreeTest, ReaderWriterIsolation_Medium) {
    BST tree;
    std::atomic<bool> done{false};
    
    const int TOTAL_ITEMS = 200;

    // 写线程：持续插入
    std::thread writer([&]() {
        std::vector<int> keys(TOTAL_ITEMS);
        for(int i=0; i<TOTAL_ITEMS; ++i) keys[i] = i * 2; // 偶数
        
        std::random_device rd;
        std::mt19937 g(rd());
        std::shuffle(keys.begin(), keys.end(), g);

        for (int val : keys) {
            STM::atomically([&](Transaction& tx) {
                tree.insert(tx, tree.root, val);
            });
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
        done = true;
    });

    // 读线程：持续读取全树快照
    std::thread reader([&]() {
        while (!done) {
            std::vector<int> snapshot;
            try {
                STM::atomically([&](Transaction& tx) {
                    snapshot.clear();
                    tree.inorder(tx, tree.root, snapshot);
                });
                
                if (!snapshot.empty()) {
                    EXPECT_TRUE(std::is_sorted(snapshot.begin(), snapshot.end()));
                }
            } catch (...) {
                // 忽略重试异常
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    writer.join();
    reader.join();

    // 清理
    std::vector<TreeNode*> garbage;
    STM::atomically([&](Transaction& tx) {
        garbage.clear();
        tree.collect_garbage(tx, tree.root, garbage);
    });
    
    STM::atomically([&](Transaction& tx) {
        for (auto* node : garbage) {
            tx.free(node);
        }
    });
}