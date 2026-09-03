#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <random>
#include <array>
#include <atomic>

#include "WwSTM/TxContext.hpp"
#include "WwSTM/TMVar.hpp"

using namespace STM::Ww;

namespace {

struct TreeNode {
    int key;
    TMVar<int>* value;
    TreeNode* left;
    TreeNode* right;

    explicit TreeNode(int k)
        : key(k), value(new TMVar<int>(0)), left(nullptr), right(nullptr) {}
};

TreeNode* buildFixedTree() {
    // Perfect BST:        4
    //                  /     \
    //                 2       6
    //                / \     / \
    //               1   3   5   7
    TreeNode* n4 = new TreeNode(4);
    TreeNode* n2 = new TreeNode(2);
    TreeNode* n6 = new TreeNode(6);
    TreeNode* n1 = new TreeNode(1);
    TreeNode* n3 = new TreeNode(3);
    TreeNode* n5 = new TreeNode(5);
    TreeNode* n7 = new TreeNode(7);

    n4->left = n2;
    n4->right = n6;
    n2->left = n1;
    n2->right = n3;
    n6->left = n5;
    n6->right = n7;

    return n4;
}

TreeNode* find(TreeNode* root, int key) {
    TreeNode* cur = root;
    while (cur) {
        if (key == cur->key) return cur;
        cur = (key < cur->key) ? cur->left : cur->right;
    }
    return nullptr;
}

void destroyTree(TreeNode* root) {
    if (!root) return;
    destroyTree(root->left);
    destroyTree(root->right);
    delete root->value;
    delete root;
}

bool incrementNode(TxContext& ctx, TreeNode* node) {
    int v = ctx.read(node->value);
    if (!ctx.isActive()) return false;
    ctx.write(node->value, v + 1);
    return ctx.isActive();
}

} // namespace

class WwSTMTreeTest : public ::testing::Test {
protected:
    TreeNode* root_ = nullptr;

    void SetUp() override {
        root_ = buildFixedTree();
    }

    void TearDown() override {
        destroyTree(root_);
        root_ = nullptr;
    }
};

TEST_F(WwSTMTreeTest, ConcurrentPathIncrement) {
    const int num_threads = 4;
    const int ops_per_thread = 1000;

    // 每次事务固定修改 3 个节点：root + mid + leaf
    // 可选叶子路径：4->2->1, 4->2->3, 4->6->5, 4->6->7
    const std::array<int, 4> leaf_keys = {1, 3, 5, 7};

    auto worker = [&](int seed) {
        std::mt19937 rng(seed);
        std::uniform_int_distribution<int> pick_leaf(0, 3);

        TxContext ctx;
        for (int i = 0; i < ops_per_thread; ++i) {
            bool committed = false;
            while (!committed) {
                ctx.begin();

                int leaf_key = leaf_keys[pick_leaf(rng)];
                int mid_key = (leaf_key < 4) ? 2 : 6;

                TreeNode* root = root_;
                TreeNode* mid = find(root_, mid_key);
                TreeNode* leaf = find(root_, leaf_key);

                ASSERT_NE(mid, nullptr);
                ASSERT_NE(leaf, nullptr);

                if (!incrementNode(ctx, root)) continue;
                if (!incrementNode(ctx, mid)) continue;
                if (!incrementNode(ctx, leaf)) continue;

                committed = ctx.commit();
            }
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(num_threads);
    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back(worker, 1234 + t * 17);
    }
    for (auto& th : threads) {
        th.join();
    }

    TxContext verify;

    int v1 = verify.read(find(root_, 1)->value);
    int v2 = verify.read(find(root_, 2)->value);
    int v3 = verify.read(find(root_, 3)->value);
    int v4 = verify.read(find(root_, 4)->value);
    int v5 = verify.read(find(root_, 5)->value);
    int v6 = verify.read(find(root_, 6)->value);
    int v7 = verify.read(find(root_, 7)->value);

    ASSERT_TRUE(verify.commit());

    const int total_ops = num_threads * ops_per_thread;
    const int sum_all = v1 + v2 + v3 + v4 + v5 + v6 + v7;

    // root 在每次事务都会被 +1
    EXPECT_EQ(v4, total_ops);

    // 每次提交只改 3 个节点
    EXPECT_EQ(sum_all, total_ops * 3);

    // 其余节点不应出现负值
    EXPECT_GE(v1, 0);
    EXPECT_GE(v2, 0);
    EXPECT_GE(v3, 0);
    EXPECT_GE(v5, 0);
    EXPECT_GE(v6, 0);
    EXPECT_GE(v7, 0);
}

TEST_F(WwSTMTreeTest, DISABLED_HighContentionProgressAndConsistency) {
    const int num_threads = 8;
    const int commits_per_thread = 3000;
    const int max_retries_per_commit = 100000;
    const std::array<int, 4> leaf_keys = {1, 3, 5, 7};

    std::atomic<int> total_commits{0};
    std::atomic<int> retry_exceeded{0};

    auto worker = [&](int seed) {
        std::mt19937 rng(seed);
        std::uniform_int_distribution<int> pick_leaf(0, 3);

        TxContext ctx;
        for (int done = 0; done < commits_per_thread; ++done) {
            bool committed = false;
            int retries = 0;

            while (!committed) {
                if (++retries > max_retries_per_commit) {
                    retry_exceeded.fetch_add(1, std::memory_order_relaxed);
                    return;
                }

                ctx.begin();
                int leaf_key = leaf_keys[pick_leaf(rng)];
                int mid_key = (leaf_key < 4) ? 2 : 6;

                TreeNode* root = root_;
                TreeNode* mid = find(root_, mid_key);
                TreeNode* leaf = find(root_, leaf_key);

                if (!incrementNode(ctx, root)) continue;
                if (!incrementNode(ctx, mid)) continue;
                if (!incrementNode(ctx, leaf)) continue;

                committed = ctx.commit();
            }
            total_commits.fetch_add(1, std::memory_order_relaxed);
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(num_threads);
    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back(worker, 9001 + t * 131);
    }
    for (auto& th : threads) {
        th.join();
    }

    ASSERT_EQ(retry_exceeded.load(std::memory_order_relaxed), 0)
        << "Detected potential livelock/starvation under high contention.";

    const int expected_total_commits = num_threads * commits_per_thread;
    ASSERT_EQ(total_commits.load(std::memory_order_relaxed), expected_total_commits);

    TxContext verify;
    int v1 = verify.read(find(root_, 1)->value);
    int v2 = verify.read(find(root_, 2)->value);
    int v3 = verify.read(find(root_, 3)->value);
    int v4 = verify.read(find(root_, 4)->value);
    int v5 = verify.read(find(root_, 5)->value);
    int v6 = verify.read(find(root_, 6)->value);
    int v7 = verify.read(find(root_, 7)->value);
    ASSERT_TRUE(verify.commit());

    const int sum_all = v1 + v2 + v3 + v4 + v5 + v6 + v7;
    EXPECT_EQ(v4, expected_total_commits);
    EXPECT_EQ(sum_all, expected_total_commits * 3);
}
