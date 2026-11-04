#pragma once

#include <atomic>
#include <cstdint>
#include "EBRManager/EBRManager.hpp"
#include "VersionNode.hpp"

namespace STM {
namespace Occ {

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
    static bool validate(const void* addr, const void* expected_head, uint64_t rv);
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
    // The owner must quiesce users before destroying the variable.
    Node* curr = head_.load(std::memory_order_acquire);
    while (curr) {
        Node* next = curr->prev.load(std::memory_order_relaxed);
        // 自动调用 VersionNode::operator delete
        delete curr; 
        curr = next;
    }
}

template<typename T>
typename TMVar<T>::Node* TMVar<T>::loadHead() const {
    return head_.load(std::memory_order_acquire);
}

// 辅助函数：级联回收链表
template<typename T>
void TMVar<T>::chainDeleter_(void* p) {
    auto* node = static_cast<Node*>(p);
    while (node) {
        // Detached chains are exclusively owned here after the EBR grace period.
        auto* next = node->prev.load(std::memory_order_relaxed);
        delete node; // 使用 VersionNode 的 delete (归还给内存池)
        node = next;
    }
}


template<typename T>
bool TMVar<T>::validate(const void* addr, const void* expected_head, uint64_t rv) {
    const auto* tmvar = static_cast<const TMVar<T>*>(addr);
    
    // 1. 获取当前 Head
    auto* current_head = tmvar->loadHead();

    // 2. 【身份检查】Head 被别人改过吗？
    if (current_head != expected_head) {
        return false; 
    }

    // 3. 【时间检查】Head 是不是“未来数据”？
    // if (current_head && current_head->write_ts > rv) {
    //     return false;
    // }
    
    return true;
}

template<typename T>
void TMVar<T>::committer(void* tmvar_ptr, void* node_ptr, uint64_t wts) {
    auto* tmvar = static_cast<TMVar<T>*>(tmvar_ptr);
    auto* new_node = static_cast<Node*>(node_ptr);
    new_node->write_ts = wts;
    
    std::atomic<Node*>& head_ref = tmvar->getHeadRef();
    // The write stripe serializes committers. Initialize the private node's
    // link before publishing it through the release store to head.
    Node* old_head = head_ref.load(std::memory_order_relaxed);
    new_node->prev.store(old_head, std::memory_order_relaxed);
    head_ref.store(new_node, std::memory_order_release);

    int depth = 0;
    Node* curr = new_node;
    
    while (curr && depth < MAX_HISTORY) {
        curr = curr->prev.load(std::memory_order_acquire);
        depth++;
    }

    if (curr) {
        // Atomically detach one tail. Readers may see the old link or nullptr;
        // neither observation permits freeing nodes before their EBR guards end.
        Node* garbage = curr->prev.exchange(nullptr, std::memory_order_acq_rel);
        if (garbage) EBRManager::instance()->retire(garbage, TMVar<T>::chainDeleter_);
    }
}

template<typename T>
void TMVar<T>::deleter(void* p) {
    if (!p) return;
    auto* node = static_cast<Node*>(p);
    // 这里的 delete 会触发：1. ~VersionNode() 2. VersionNode::operator delete()
    delete node;
}

} // namespace Occ
} // namespace STM
