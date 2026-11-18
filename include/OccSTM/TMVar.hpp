#pragma once

#include <atomic>
#include <cstdint>
#include <new>
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
    // No allocation, pruning or throwing is permitted during publication.
    static void committer(void* tmvar_ptr, void* node_ptr, uint64_t wts) noexcept;
    static void pruneNoThrow(void* tmvar_ptr) noexcept;
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
bool TMVar<T>::validate(const void* addr, const void* selected_node, uint64_t rv) {
    const auto* tmvar = static_cast<const TMVar<T>*>(addr);
    Node* current_head = tmvar->loadHead();
    // An updating transaction retains strict OCC semantics: it cannot
    // commit based on a historical value that is no longer the head.
    return current_head == selected_node &&
           current_head != nullptr && current_head->write_ts <= rv;
}

template<typename T>
void TMVar<T>::committer(void* tmvar_ptr, void* node_ptr, uint64_t wts) noexcept {
    auto* tmvar = static_cast<TMVar<T>*>(tmvar_ptr);
    auto* new_node = static_cast<Node*>(node_ptr);

    // The writer owns the stripe throughout the entire multi-var commit.
    // This critical publication path is strictly non-throwing.
    new_node->write_ts = wts;
    Node* old_head = tmvar->head_.load(std::memory_order_relaxed);
    new_node->prev.store(old_head, std::memory_order_relaxed);
    tmvar->head_.store(new_node, std::memory_order_release);
}

template<typename T>
void TMVar<T>::pruneNoThrow(void* tmvar_ptr) noexcept {
    // Called after all heads have been published, but *before* any of the
    // transaction's versioned write locks are released. This is optional
    // maintenance: if the retirement metadata cannot be allocated, simply
    // retain more history for a later commit.
    auto* tmvar = static_cast<TMVar<T>*>(tmvar_ptr);
    Node* curr = tmvar->head_.load(std::memory_order_relaxed);

    for (int depth = 0; curr && depth < MAX_HISTORY; ++depth) {
        curr = curr->prev.load(std::memory_order_relaxed);
    }
    if (!curr || !curr->prev.load(std::memory_order_relaxed)) return;

    // Allocate the EBR ledger *before* disconnecting the old chain.
    // Once the link is severed, there must be no further fallible work.
    GarbageNode* ledger =
        new (std::nothrow) GarbageNode(nullptr, TMVar<T>::chainDeleter_);
    if (!ledger) return;

    Node* retired = curr->prev.exchange(nullptr, std::memory_order_acq_rel);
    if (!retired) {
        delete ledger;
        return;
    }
    ledger->garbage_ptr = retired;
    EBRManager::instance()->retirePrepared(ledger);
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
