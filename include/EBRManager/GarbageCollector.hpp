#pragma once

#include "EBRManager/GarbageNode.hpp"

/**
 * @class GarbageCollector
 * @brief Reclaim a detached, exclusively owned list of retired objects.
 *
 * Each collect() call owns its input list after the epoch manager has stolen
 * an expired bucket. Calls on distinct lists can therefore run concurrently:
 * no shared collector state needs a global mutex.
 *
 * The EBR grace period ensures that readers no longer hold retired objects,
 * but it does not serialize arbitrary user callbacks. Custom deleters and
 * payload destructors must be safe to invoke concurrently with other
 * independent reclamation callbacks; each retired object must appear in
 * exactly one detached list. Reentrant collection of another private list
 * is permitted.
 *
 * GarbageNode metadata is allocated with ::operator new, independently from
 * the payload allocator, and is released with the matching ::operator delete.
 */
class GarbageCollector {
public:
    using Node = GarbageNode;

    GarbageCollector() = default;
    ~GarbageCollector() = default;

    GarbageCollector(const GarbageCollector&) = delete;
    GarbageCollector& operator=(const GarbageCollector&) = delete;
    GarbageCollector(GarbageCollector&&) = delete;
    GarbageCollector& operator=(GarbageCollector&&) = delete;

    void collect(Node* garbage_list_head);
};


inline void GarbageCollector::collect(Node* garbage_list_head) {
    // The caller exclusively owns every node in this detached list; neither
    // bucket manipulation nor another collection can reach these nodes.
    Node* current = garbage_list_head;
    while (current != nullptr) {
        Node* next = current->next;

        // GarbageNode::~GarbageNode invokes the payload's deleter.
        current->~Node();
        ::operator delete(current);

        current = next;
    }
}
