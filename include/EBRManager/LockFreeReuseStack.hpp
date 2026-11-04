#pragma once

#include <mutex>

// Historical name retained for compatibility. Slot acquisition/release is a
// thread-lifecycle path: serialize both the head and intrusive next accesses.
// A tagged head alone does not protect next when a popped node is reused, and
// a finite stamp does not provide an unbounded ABA guarantee.
template <typename Node>
class LockFreeReuseStack {
private:
    Node* head_ = nullptr;
    std::mutex mutex_;

public:
    LockFreeReuseStack() = default;
    ~LockFreeReuseStack() = default;

    LockFreeReuseStack(const LockFreeReuseStack&) = delete;
    LockFreeReuseStack& operator=(const LockFreeReuseStack&) = delete;
    LockFreeReuseStack(LockFreeReuseStack&&) = delete;
    LockFreeReuseStack& operator=(LockFreeReuseStack&&) = delete;

    void push(Node* new_node);
    Node* pop() noexcept;
};


template <typename Node>
void LockFreeReuseStack<Node>::push(Node* new_node) {
    if (!new_node) return;
    std::lock_guard<std::mutex> lock(mutex_);
    new_node->next = head_;
    head_ = new_node;
}

template <typename Node>
Node* LockFreeReuseStack<Node>::pop() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    Node* node = head_;
    if (node) head_ = node->next;
    return node;
}
