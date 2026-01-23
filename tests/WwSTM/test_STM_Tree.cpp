#include <gtest/gtest.h>
#include <iostream>
#include <thread>
#include <vector>
#include <atomic>
#include <mutex>
#include <set>
#include <unordered_set>
#include <random>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <chrono>

// 引入核心头文件 (假设路径正确)
#include "WwSTM/TxContext.hpp"
#include "WwSTM/TMVar.hpp"

using namespace STM::Ww;

namespace {

// =========================================================
// 0. 调试工具 (增加全局锁方便打印树)
// =========================================================
std::mutex g_io_mutex; // 全局IO锁，防止打印错乱

class DebugLogger {
    using Clock = std::chrono::steady_clock;
    Clock::time_point start_time;
public:
    static DebugLogger& get() { static DebugLogger instance; return instance; }
    DebugLogger() { start_time = Clock::now(); }
    
    void log(const std::string& msg) {
        // 在这一步不加锁，尽量减少对并发调度的干扰，输出乱一点没关系
        auto diff = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start_time).count();
        std::stringstream ss;
        ss << "[T" << std::hash<std::thread::id>{}(std::this_thread::get_id()) % 1000 
           << " | " << std::setw(8) << diff << "us] " << msg << "\n";
        std::cout << ss.str(); // cout 本身是线程安全的字符流
    }
};
#define LOG(msg) { std::stringstream ss; ss << msg; DebugLogger::get().log(ss.str()); }

// =========================================================
// 1. 结构定义
// =========================================================
static std::atomic<int> g_node_id_counter{0};

struct SimpleNode {
    int id;
    int value;
    TMVar<SimpleNode*>* left;
    TMVar<SimpleNode*>* right;

    SimpleNode(int v) : value(v) {
        id = ++g_node_id_counter;
        left = new TMVar<SimpleNode*>(nullptr);
        right = new TMVar<SimpleNode*>(nullptr);
    }
    
    // 简单的递归析构逻辑（注意：如果有环，这里如果是默认 delete 会栈溢出，所以仅用于非环结构）
    // 在本测试中，我们手动管理内存，不依赖析构函数级联删除
    ~SimpleNode() {}
};

void destroy_uncommitted_node(SimpleNode* node) {
    if (!node) return;
    delete node->left;
    delete node->right;
    delete node;
}

// =========================================================
// 2. 树操作封装
// =========================================================
class SimpleTree {
public:
    TMVar<SimpleNode*>* root_var;

    SimpleTree() {
        root_var = new TMVar<SimpleNode*>(nullptr);
    }

    ~SimpleTree() {
        // Root var is deleted in TearDown
    }

    bool insert(TxContext& tx, int key, SimpleNode** out_created_node = nullptr) {
        TMVar<SimpleNode*>* curr_var = root_var;
        int depth = 0;

        while (true) {
            if (depth++ > 20) throw std::runtime_error("Tree depth limit reached (Cycle?)");

            SimpleNode* curr_node = tx.read(*curr_var);

            if (!tx.isActive()) throw std::runtime_error("Tx aborted during read");

            if (curr_node == nullptr) {
                SimpleNode* new_node = new SimpleNode(key);
                if (out_created_node) *out_created_node = new_node;
                
                tx.write(*curr_var, new_node);
                
                if (!tx.isActive()) throw std::runtime_error("Tx aborted during write");
                return true; 
            }

            if (curr_node->value == key) return false; // Duplicate

            if (key < curr_node->value) curr_var = curr_node->left;
            else curr_var = curr_node->right;
        }
    }
};

} // namespace

// =========================================================
// 3. 测试夹具
// =========================================================
class WwSTMCleanupTest : public ::testing::Test {
protected:
    SimpleTree* tree;

    void SetUp() override { 
        g_node_id_counter = 0;
        tree = new SimpleTree(); 
    }

    // 安全的 TearDown，防止因为环导致无限递归或 Segfault
    void TearDown() override {
        LOG("==== TearDown Started ====");
        if (tree) {
            std::unordered_set<TMVar<SimpleNode*>*> visited_vars;
            cleanup_recursive_safe(tree->root_var, visited_vars);
            delete tree;
        }
        LOG("==== TearDown Finished ====");
    }

    // 修复后的递归清理：带 visited 检查
    void cleanup_recursive_safe(TMVar<SimpleNode*>* var, std::unordered_set<TMVar<SimpleNode*>*>& visited) {
        if (!var) return;
        
        // 1. 查重：如果这个 TMVar 已经访问过，说明有环或共享引用
        if (visited.count(var)) {
            LOG("WARNING: Cycle/Shared TMVar detected at cleanup @" << (void*)var << ". Skipping double-free.");
            return;
        }
        visited.insert(var);

        SimpleNode* node = nullptr;
        // 2. 尝试读取 (非事务读取，假设此时无并发)
        {
            TxContext tx; 
            try {
                node = tx.read(*var); // 这里可能需要适配你的 API
            } catch (...) {}
        }

        if (node) {
            cleanup_recursive_safe(node->left, visited);
            cleanup_recursive_safe(node->right, visited);
            delete node; // 只有这里才真正删除 Node
        }
        delete var; // 删除 TMVar
    }

    // ==========================================
    // 调试辅助：打印树并检测环
    // ==========================================
    void dump_tree_and_check(SimpleNode* node, int depth, std::set<SimpleNode*>& visited_nodes, std::ostream& oss, bool& is_graph) {
        if (!node) {
            oss << "null";
            return;
        }

        // 打印当前节点信息
        oss << "{ID:" << node->id << " V:" << node->value << " @" << (void*)node << "}";

        // 完整性检查：节点是否已在路径上
        if (visited_nodes.count(node)) {
            oss << " <--- CYCLE DETECTED HERE!";
            is_graph = true;
            return;
        }
        visited_nodes.insert(node);

        // 递归打印左子树
        oss << "\n";
        for(int i=0; i<depth+1; ++i) oss << "  |";
        oss << "L-> ";
        
        // 开启 ReadOnly 事务读取子节点
        SimpleNode* left_child = nullptr;
        SimpleNode* right_child = nullptr;
        {
            TxContext tx;
            try {
                if(node->left) left_child = tx.read(*(node->left));
                if(node->right) right_child = tx.read(*(node->right));
            } catch(...) { oss << "[ReadErr]"; }
        }

        dump_tree_and_check(left_child, depth + 1, visited_nodes, oss, is_graph);

        // 递归打印右子树
        oss << "\n";
        for(int i=0; i<depth+1; ++i) oss << "  |";
        oss << "R-> ";
        dump_tree_and_check(right_child, depth + 1, visited_nodes, oss, is_graph);
    }
};

// =========================================================
// 4. 测试用例 (低压调试版)
// =========================================================
TEST_F(WwSTMCleanupTest, Debug_GraphDetection) {
    // 降低压力以便观察日志
    const int NUM_THREADS = 2;   // 只有2个线程竞争，最容易看清 Race
    const int OPS_PER_THREAD = 20; 
    const int KEY_RANGE = 20; 

    std::atomic<bool> stop_flag{false}; // 一旦发现结构崩坏，停止所有线程

    auto worker = [&](int tid) {
        std::mt19937 rng(tid * 999 + 1); 
        std::uniform_int_distribution<int> dist(0, KEY_RANGE);
        TxContext tx;

        for (int i = 0; i < OPS_PER_THREAD; ++i) {
            if (stop_flag) break;

            int key = dist(rng);
            SimpleNode* temp_node = nullptr;
            bool success = false;
            
            // --- 事务操作 ---
            tx.begin();
            try {
                tree->insert(tx, key, &temp_node);
                if (tx.commit()) {
                    success = true;
                    temp_node = nullptr; // 所有权移交给树
                } else {
                    destroy_uncommitted_node(temp_node);
                }
            } catch (...) {
                destroy_uncommitted_node(temp_node);
            }

            // --- 成功后立即检查并打印 (串行化部分) ---
            if (success) {
                std::lock_guard<std::mutex> lock(g_io_mutex);
                
                // 为了看清是哪一步坏的，每次成功插入都检查一次
                LOG("Thread " << tid << " inserted key " << key << ". Checking integrity...");

                TxContext verify_tx;
                verify_tx.begin(); // 获取一致性快照
                SimpleNode* root = verify_tx.read(*(tree->root_var));

                std::stringstream tree_dump;
                std::set<SimpleNode*> history;
                bool is_graph = false;
                
                tree_dump << "\nROOT -> ";
                dump_tree_and_check(root, 0, history, tree_dump, is_graph);
                
                verify_tx.commit(); // 结束只读事务

                if (is_graph) {
                    LOG("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
                    LOG("FATAL ERROR: Graph structure detected (Cycle or Shared Node)!");
                    LOG("Structure Dump:\n" << tree_dump.str());
                    LOG("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
                    stop_flag = true;
                } else {
                    // 如果你想看每一步的树，取消下面这行的注释
                     LOG(tree_dump.str()); 
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10)); // 故意慢一点
        }
    };

    LOG("Starting debug threads...");
    std::vector<std::thread> threads;
    for(int i=0; i<NUM_THREADS; ++i) threads.emplace_back(worker, i);
    for(auto& t : threads) t.join();

    if (stop_flag) {
        FAIL() << "Test failed because Tree corruption (Graph) was detected.";
    }
}