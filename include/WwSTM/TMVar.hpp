#pragma once

#include <atomic>
#include <cstdint>
#include <type_traits>

#include "TaggedPtr.hpp"
#include "VersionNode.hpp"
#include "WriteRecord.hpp"
#include "EBRManager/EBRManager.hpp"

namespace STM {
namespace Ww {

// 为了让 TxContext::write_set_ 可以通过 std::vector<TMVarBase*> 统一存储不同类型的变量
struct TMVarBase {
    virtual ~TMVarBase() = default;

    virtual uintptr_t load() const = 0;
    virtual bool compareExchange(uintptr_t& expected, uintptr_t desired) = 0;
};


template <typename T>
class TMVar : public TMVarBase{
public:
    using NodeT = detail::VersionNode<T>;
    using RecordT = detail::WriteRecord<T>;

private:
    std::atomic<uintptr_t> atomic_ptr_;

public:
    template<typename... Args>
    TMVar(Args&&...args) {
        NodeT* init_node = new NodeT(0, std::forward<Args>(args)...);
        uintptr_t raw = TaggedPtrHelper::packNode(init_node);
        atomic_ptr_.store(raw, std::memory_order_release);
    }

    ~TMVar() {
        uintptr_t current = atomic_ptr_.load(std::memory_order_acquire);
        // 当前事务变量存储的时record（有人正在读写）
        if(TaggedPtrHelper::isRecord(current)) {
            RecordT* record = TaggedPtrHelper::unpackRecord<RecordT>(current);
            EBRManager::instance()->retire(record);
        }
        // 当前事务变量存储的是稳定的 node
        else {
            NodeT* node = TaggedPtrHelper::unpackNode<NodeT>(current);
            EBRManager::instance()->retire(node);
        }
    }

    // 禁止拷贝和移动
    TMVar(const TMVar&) = delete;
    TMVar& operator=(const TMVar&) = delete;
    TMVar(TMVar&&) = delete;
    TMVar& operator=(TMVar&&) = delete;

    uintptr_t load() const override {
        return atomic_ptr_.load(std::memory_order_acquire);
    }

    bool compareExchange(uintptr_t& expected, uintptr_t desired) override {
        // 使用 acq_rel 内存序，保证读写的同步语义
        return atomic_ptr_.compare_exchange_strong(
            expected, 
            desired, 
            std::memory_order_acq_rel
        );
    }

    bool isOwned() const {
        return TaggedPtrHelper::isRecord(load());
    }
};


}
}