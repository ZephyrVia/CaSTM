#pragma once

#include <atomic>
#include <cstdint>
#include "EBRManager/EBRManager.hpp"
#include "VersionNode.hpp"


template<typename T>
class TMVar {
public:
    using Node = detail::VersionNode<T>; 

    template<typename... Args>
    explicit TMVar(Args&&... args);
    
    ~TMVar();

    std::atomic<Node*>& getHeadRef() { return head_; }
    Node* loadHead() const;

    static constexpr int MAX_HISTORY = 8;

    // 静态生命周期管理函数
    static bool validate(const void* addr, uint64_t rv);
    static void committer(void* tmvar_ptr, void* node_ptr, uint64_t wts);
    static void deleter(void* p);

    TMVar(const TMVar&) = delete;    
    TMVar& operator= (const TMVar&) = delete;

private:
    static void chainDeleter_(void* node);

private:
    std::atomic<Node*> head_{nullptr};
};


template<typename T>
template<typename... Args>
TMVar<T>::TMVar(Args&&... args) {
    // 自动调用 VersionNode::operator new
    Node* init_node = new Node(0, nullptr, std::forward<Args>(args)...);
    head_.store(init_node, std::memory_order_release);
}

template<typename T>
TMVar<T>::~TMVar() {
    Node* curr = head_.load(std::memory_order_acquire);
    while (curr) {
        Node* next = curr->prev;
        // 自动调用 VersionNode::operator delete
        delete curr; 
        curr = next;
    }
}

template<typename T>
typename TMVar<T>::Node* TMVar<T>::loadHead() const {
    return head_.load(std::memory_order_acquire);
}

// template<typename T>
// bool TMVar<T>::validate(const void* addr, uint64_t rv) {
//     const auto* tmvar = static_cast<const TMVar<T>*>(addr);
//     auto* head = tmvar->loadHead();

//     // 如果 head 为空，说明 TMVar 还没有任何数据，或者数据被完全清空，认为不冲突
//     if (head == nullptr) return true; 

//     // 检查head是否在新版本
//     // 如果head的写时间戳都比当前读版本rv要大，说明在rv时刻，head版本不存在，
//     // head是最新版本，且在rv时刻无可见版本
//     // 而Transaction::load会遍历旧版本，所以这里必须检查head本身
//     if (head->write_ts > rv) { 
//         // 找到在rv时刻，head的那个版本，必须遍历
//         auto* curr = head;
//         while(curr != nullptr && curr->write_ts > rv) {
//             curr = curr->prev;
//         }
//         // 如果遍历完了，curr是空，说明在rv时刻没有可见版本
//         if (curr == nullptr) return false; // 冲突了，旧版本都被回收了或者版本过新
//     }
    
//     // 如果head的写时间戳小于等于rv，说明head是可见的，且是rv时刻最新的版本
//     // 此时我们只需要检查head的写时间戳是否被改变
//     // 也就是说，这个TMVar在tx开始后到提交前，是否被其他事务修改并提交了
//     return head->write_ts <= rv;
// }

template<typename T>
bool TMVar<T>::validate(const void* addr, uint64_t rv) {
    const auto* tmvar = static_cast<const TMVar<T>*>(addr);
    auto* head = tmvar->loadHead();

    // 情况1: 变量为空，没有冲突
    if (head == nullptr) return true; 

    // 情况2: 严格的 TL2 验证
    // 如果 Head 的写入时间戳大于我的读版本 (RV)，说明在我开始事务后，
    // 已经有其他事务修改并提交了这个变量。
    // 无论历史版本是否存在，为了保证 Read-Modify-Write 的原子性，
    // 这里必须返回 false (Abort)，强制当前事务重试并读取最新值。
    if (head->write_ts > rv) {
        return false;
    }
    
    return true;
}

template<typename T>
void TMVar<T>::committer(void* tmvar_ptr, void* node_ptr, uint64_t wts) {
    auto* tmvar = static_cast<TMVar<T>*>(tmvar_ptr);
    auto* new_node = static_cast<Node*>(node_ptr);
    new_node->write_ts = wts;
    
    std::atomic<Node*>& head_ref = tmvar->getHeadRef();
    // 1. 正常的挂链（Relaxed 因为我们在锁内）
    Node* old_head = head_ref.load(std::memory_order_relaxed);
    new_node->prev = old_head;
    head_ref.store(new_node, std::memory_order_release);

    int depth = 0;
    Node* curr = new_node;
    
    while (curr && depth < MAX_HISTORY) {
        curr = curr->prev;
        depth++;
    }

    if (curr && curr->prev) {
        Node* garbage = curr->prev;
        curr->prev = nullptr;   // 关键步骤：逻辑斩断！

        EBRManager::instance()->retire(garbage, TMVar<T>::chainDeleter_);  // 现在的 garbage 才是真正安全的回收对象
    }
}

// 辅助函数：级联回收链表
template<typename T>
void TMVar<T>::chainDeleter_(void* p) {
    auto* node = static_cast<Node*>(p);
    while (node) {
        auto* next = node->prev;
        delete node; // 使用 VersionNode 的 delete (归还给内存池)
        node = next;
    }
}

template<typename T>
void TMVar<T>::deleter(void* p) {
    if (!p) return;
    auto* node = static_cast<Node*>(p);
    // 这里的 delete 会触发：1. ~VersionNode() 2. VersionNode::operator delete()
    delete node;
}