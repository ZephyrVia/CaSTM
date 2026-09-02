#include "EBRManager/LockFreeSingleLinkedList.hpp"


LockFreeSingleLinkedList::LockFreeSingleLinkedList() {
    head_ = nullptr;
}

void LockFreeSingleLinkedList::pushNode(Node* new_node) {
    if (!new_node) return;
    std::lock_guard<std::mutex> lk(mu_);
    new_node->next = head_;
    head_ = new_node;
}

LockFreeSingleLinkedList::Node* LockFreeSingleLinkedList::stealList() noexcept {
    std::lock_guard<std::mutex> lk(mu_);
    Node* old_head = head_;
    head_ = nullptr;
    return old_head;
}
