#pragma once

#include <atomic>
#include <mutex>
#include "EBRManager/GarbageNode.hpp"

class LockFreeSingleLinkedList {
public:
    using Node = GarbageNode;

private:
    Node* head_;
    mutable std::mutex mu_;

public:
    LockFreeSingleLinkedList();
    ~LockFreeSingleLinkedList() = default;

    LockFreeSingleLinkedList(const LockFreeSingleLinkedList&) = delete;
    LockFreeSingleLinkedList& operator=(const LockFreeSingleLinkedList&) = delete;
    LockFreeSingleLinkedList(LockFreeSingleLinkedList&&) = delete;
    LockFreeSingleLinkedList& operator=(LockFreeSingleLinkedList&&) = delete;

    void pushNode(Node* new_node);
    Node* stealList() noexcept;
};
