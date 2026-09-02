#pragma once

#include <mutex>
#include "EBRManager/GarbageNode.hpp"

// 注：原实现为带 stamp 的无锁 Treiber 栈，16 位 stamp 在高频 push/steal 下
// 存在回绕 ABA 隐患，且收益有限（每纪元至多一次 steal）。改为互斥锁实现，
// 正确性优先；如需去锁可后续基于 64 位带指针计数器重写。
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
