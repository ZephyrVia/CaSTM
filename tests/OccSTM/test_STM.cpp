#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <stdexcept>
#include <algorithm>
#include "OccSTM/STM.hpp"

using namespace STM::Occ;

// ==========================================
// 1. 基础功能测试：验证 load/store 和返回值逻辑
// ==========================================
TEST(STMTest, BasicReadWrite) {
    STM::Var<int> account(100);

    // 测试 void 类型的 atomically
    STM::atomically([&](Transaction& tx) {
        int val = tx.load(account);
        tx.store(account, val + 50);
    });

    // 测试带返回值的 atomically
    int current_balance = STM::atomically([&](Transaction& tx) {
        return tx.load(account);
    });

    EXPECT_EQ(current_balance, 150);
}

// ==========================================
// 2. 异常回滚测试：验证抛出异常后，修改不会生效
// ==========================================
TEST(STMTest, ExceptionRollback) {
    STM::Var<std::string> status("Clean");

    // 预期会抛出 runtime_error
    EXPECT_THROW({
        STM::atomically([&](Transaction& tx) {
            tx.store(status, std::string("Dirty")); // 尝试修改
            throw std::runtime_error("Boom!"); // 抛出异常，触发回滚
        });
    }, std::runtime_error);

    // 验证状态是否保持原样
    std::string result = STM::atomically([&](Transaction& tx) {
        return tx.load(status);
    });

    EXPECT_EQ(result, "Clean");
}

// ==========================================
// 3. 并发测试：多线程累加器
// ==========================================
TEST(STMTest, ConcurrentCounter) {
    STM::Var<int> counter(0);
    
    const int NUM_THREADS = 8;
    const int INC_PER_THREAD = 1000;

    std::vector<std::thread> workers;
    
    // 启动线程
    for (int i = 0; i < NUM_THREADS; ++i) {
        workers.emplace_back([&]() {
            for (int j = 0; j < INC_PER_THREAD; ++j) {
                STM::atomically([&](Transaction& tx) {
                    // 经典的 Read-Modify-Write 竞争场景
                    int val = tx.load(counter);
                    tx.store(counter, val + 1);
                });
            }
        });
    }

    // 等待结束
    for (auto& t : workers) {
        t.join();
    }

    // 验证最终结果
    int final_val = STM::atomically([&](Transaction& tx) {
        return tx.load(counter);
    });

    // 如果没有 Occ 冲突检测，这个值会小于 8000
    EXPECT_EQ(final_val, NUM_THREADS * INC_PER_THREAD);
}

// ==========================================
// 4. 并发有序链表测试
// ==========================================

// 定义链表节点结构
struct ListNode {
    int val;
    STM::Var<ListNode*> next;

    ListNode(int v) : val(v), next(nullptr) {}
};

TEST(STMTest, ConcurrentOrderedList) {
    // 链表头指针
    STM::Var<ListNode*> head(nullptr);

    // 配置：4个线程，每个线程插入50个节点
    const int NUM_THREADS = 4;
    const int ITEMS_PER_THREAD = 50;

    std::vector<std::thread> workers;

    // 启动线程
    for (int i = 0; i < NUM_THREADS; ++i) {
        workers.emplace_back([&, i]() {
            for (int j = 0; j < ITEMS_PER_THREAD; ++j) {
                int val_to_insert = j * NUM_THREADS + i;

                STM::atomically([&](Transaction& tx) {
                    // 使用 tx.alloc 替代 new
                    ListNode* new_node = tx.alloc<ListNode>(val_to_insert);

                    // 寻找插入位置 (prev -> new_node -> curr)
                    ListNode* prev = nullptr;
                    ListNode* curr = tx.load(head);

                    while (curr != nullptr) {
                        if (curr->val > val_to_insert) {
                            break;
                        }
                        prev = curr;
                        curr = tx.load(curr->next);
                    }

                    // 执行链接操作
                    tx.store(new_node->next, curr);

                    if (prev == nullptr) {
                        tx.store(head, new_node);
                    } else {
                        tx.store(prev->next, new_node);
                    }
                });
            }
        });
    }

    // 等待所有插入完成
    for (auto& t : workers) {
        t.join();
    }

    // ==========================================
    // 验证阶段 (Verify)
    // ==========================================
    STM::atomically([&](Transaction& tx) {
        ListNode* curr = tx.load(head);
        int count = 0;
        int last_val = -1;

        while (curr != nullptr) {
            // 验证有序性
            if (curr->val <= last_val) {
                ADD_FAILURE() << "List is NOT sorted! Found " << curr->val << " after " << last_val;
            }
            last_val = curr->val;
            count++;
            curr = tx.load(curr->next);
        }

        // 验证完整性
        EXPECT_EQ(count, NUM_THREADS * ITEMS_PER_THREAD) 
            << "List size mismatch! Possible Lost Insert.";
    });

    // ==========================================
    // 清理内存 (防止 ASan 报错)
    // ==========================================
    std::vector<ListNode*> nodes_to_delete;
    STM::atomically([&](Transaction& tx) {
        nodes_to_delete.clear(); // 若重试需清空
        ListNode* curr = tx.load(head);
        while (curr) {
            nodes_to_delete.push_back(curr);
            curr = tx.load(curr->next);
        }
        // 断开引用
        tx.store(head, (ListNode*)nullptr);
    });

    // 【修改点】使用 STM::atomically 包裹 tx.free 调用
    // 这样就使用了 Transaction 内部封装的释放逻辑 (ThreadHeap::deallocate)
    STM::atomically([&](Transaction& tx) {
        for (auto* node : nodes_to_delete) {
            tx.free(node);
        }
    });
}

// ==========================================
// 5. 并发二叉搜索树 (BST) 测试
// ==========================================

// 定义树节点
struct TreeNode {
    int val;
    STM::Var<TreeNode*> left;
    STM::Var<TreeNode*> right;

    TreeNode(int v) : val(v), left(nullptr), right(nullptr) {}
};

// 辅助函数：中序遍历
void inorder_traversal(Transaction& tx, TreeNode* node, std::vector<int>& result) {
    if (!node) return;
    
    TreeNode* left_child = tx.load(node->left);
    inorder_traversal(tx, left_child, result);
    
    result.push_back(node->val);
    
    TreeNode* right_child = tx.load(node->right);
    inorder_traversal(tx, right_child, result);
}

// 辅助函数：收集所有节点指针以便删除
void collect_nodes(Transaction& tx, TreeNode* node, std::vector<TreeNode*>& out) {
    if (!node) return;
    collect_nodes(tx, tx.load(node->left), out);
    collect_nodes(tx, tx.load(node->right), out);
    out.push_back(node);
}

TEST(STMTest, ConcurrentBST) {
    STM::Var<TreeNode*> root(nullptr);

    // 配置：4线程，每线程插入 50 个节点
    const int NUM_THREADS = 4;
    const int ITEMS_PER_THREAD = 50; 
    
    std::vector<std::thread> workers;

    for (int i = 0; i < NUM_THREADS; ++i) {
        workers.emplace_back([&, i]() {
            for (int j = 0; j < ITEMS_PER_THREAD; ++j) {
                // 生成唯一键值，使用间隔插入增加竞争
                int val_to_insert = i + j * NUM_THREADS; 

                STM::atomically([&](Transaction& tx) {
                    // 使用 tx.alloc 替代 new
                    // 自动管理未提交内存的生命周期
                    TreeNode* new_node = tx.alloc<TreeNode>(val_to_insert);
                    
                    // 1. 处理根节点
                    TreeNode* curr = tx.load(root);
                    if (curr == nullptr) {
                        tx.store(root, new_node);
                        return;
                    }

                    // 2. 寻找插入位置
                    while (true) {
                        if (val_to_insert < curr->val) {
                            // 向左走
                            TreeNode* left = tx.load(curr->left);
                            if (left == nullptr) {
                                tx.store(curr->left, new_node);
                                break;
                            }
                            curr = left;
                        } else {
                            // 向右走
                            TreeNode* right = tx.load(curr->right);
                            if (right == nullptr) {
                                tx.store(curr->right, new_node);
                                break;
                            }
                            curr = right;
                        }
                    }
                });
            }
        });
    }

    for (auto& t : workers) {
        t.join();
    }

    // ==========================================
    // 验证阶段
    // ==========================================
    STM::atomically([&](Transaction& tx) {
        std::vector<int> sorted_vals;
        TreeNode* root_node = tx.load(root);
        
        inorder_traversal(tx, root_node, sorted_vals);

        // 1. 验证数量
        EXPECT_EQ(sorted_vals.size(), NUM_THREADS * ITEMS_PER_THREAD)
            << "Tree size mismatch! Lost updates detected.";

        // 2. 验证有序性
        bool is_sorted = std::is_sorted(sorted_vals.begin(), sorted_vals.end());
        EXPECT_TRUE(is_sorted) << "Tree does not maintain BST property!";
        
        // 验证无重复
        auto last = std::unique(sorted_vals.begin(), sorted_vals.end());
        EXPECT_EQ(last, sorted_vals.end()) << "Duplicate values found in tree!";
    });

    // ==========================================
    // 清理内存 (防止 ASan 报错)
    // ==========================================
    std::vector<TreeNode*> nodes_to_delete;
    STM::atomically([&](Transaction& tx) {
        nodes_to_delete.clear();
        TreeNode* root_node = tx.load(root);
        collect_nodes(tx, root_node, nodes_to_delete);
        // 断开根引用
        tx.store(root, (TreeNode*)nullptr);
    });

    // 【修改点】使用 STM::atomically 包裹 tx.free 调用
    STM::atomically([&](Transaction& tx) {
        for (auto* node : nodes_to_delete) {
            tx.free(node);
        }
    });
}

