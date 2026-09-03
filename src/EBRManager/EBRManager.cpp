// EBRManager.cpp
#include "EBRManager/EBRManager.hpp"
#include "EBRManager/ThreadSlot.hpp"

EBRManager::EBRManager() {
    // 初始化全局纪元为0
    global_epoch_.store(0, std::memory_order_relaxed);
}

EBRManager::~EBRManager() {
    for (size_t list_index = 0; list_index < kNumEpochLists; ++list_index) {
        collectGarbage_(list_index);
    }
}

ThreadSlot* EBRManager::getLocalSlot_() {
    return slot_manager_.getLocalSlot();
}

void EBRManager::enter() {
    ThreadSlot* slot = getLocalSlot_();
    if (slot) {
        // 登记-复核循环：在“读取全局纪元”与“登记到槽位”两步之间，
        // 其他线程可能连续推进纪元并完成回收。若以过期纪元登记，
        // 本线程会错过它本应挡住的推进。复核并前移到最新纪元
        // （前移只发生在本线程任何读取之前，方向安全）。
        for (int i = 0; i < 4; ++i) {
            uint64_t current_epoch = global_epoch_.load(std::memory_order_acquire);
            slot->enter(current_epoch);
            uint64_t now = global_epoch_.load(std::memory_order_acquire);
            if (now == current_epoch) break;
            slot->setEpoch(now);
        }
    }
}


void EBRManager::leave() {
    ThreadSlot* slot = getLocalSlot_();
    if (slot) {
        // 标记线程离开临界区（变为非活跃状态）
        slot->leave();

        if (tryAdvanceEpoch_()) {
            uint64_t current_global_epoch = global_epoch_.load(std::memory_order_relaxed);

            if (current_global_epoch >= 2) {
                uint64_t epoch_to_collect = current_global_epoch - 2;
                collectGarbage_(epoch_to_collect);
            }
        }
    }
}

bool EBRManager::tryAdvanceEpoch_() {
    // 使用 acquire 内存序加载，确保我们能看到其他线程 leave 操作释放的最新状态
    uint64_t current_epoch = global_epoch_.load(std::memory_order_acquire);
    
    bool can_advance = true;

    // 遍历所有已注册的线程槽，检查是否有“掉队者”
    slot_manager_.forEachSlot([&](const ThreadSlot& slot) {
        if (!can_advance) return; // 如果已发现不能推进，提前退出

        uint64_t slot_state = slot.loadState();

        if (ThreadSlot::isActive(slot_state) && 
            ThreadSlot::unpackEpoch(slot_state) < current_epoch) {
            can_advance = false;
        }
    });

    if (!can_advance) {
        return false; // 发现掉队者，无法推进
    }

    // 如果没有掉队者，尝试原子地将全局纪元加一
    return global_epoch_.compare_exchange_strong(
        current_epoch, 
        current_epoch + 1,
        std::memory_order_acq_rel,
        std::memory_order_relaxed
    );
}

void EBRManager::collectGarbage_(uint64_t epoch_to_collect) {
    size_t list_index = epoch_to_collect % kNumEpochLists;

    GarbageNode* garbage_head = garbage_lists_[list_index].stealList();

    if (garbage_head) {
        garbage_collector_.collect(garbage_head);
    }
}

void EBRManager::retire(void* ptr, void (*deleter)(void*)) {
    if(ptr == nullptr) return;

    // GarbageNode 是 EBR 自己的回收账本，必须放在系统堆：
    // 若从 ThreadHeap 分配，节点会随分配线程的 slab 生命周期漂移，
    // 正是“回收账本被复用内存覆盖”的通道。
    void* gnode_mem = ::operator new(sizeof(GarbageNode));
    GarbageNode* g_node = new(gnode_mem) GarbageNode(ptr, deleter);

    // 退休发生时以全局纪元作为回收账本的时间戳，而不是使用本线程
    // announced epoch。线程可能仍停留在 E，但全局纪元已经推进到 E+1；
    // 若把对象放入 E 桶，collect(G-2) 会在 G=E+2 时提前回收它。
    uint64_t retire_epoch = global_epoch_.load(std::memory_order_acquire);
    this->garbage_lists_[retire_epoch % kNumEpochLists].pushNode(g_node);
}
