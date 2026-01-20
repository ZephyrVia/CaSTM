#pragma once

#include <atomic>
#include <cstdint>

namespace STM {
namespace Ww {

// 事务状态枚举
enum class TxStatus : uint8_t {
    ACTIVE = 0,     // 事务正在运行 
    COMMITTED = 1,  // 事务已提交
    ABORTED = 2     // 事务已回滚
};

struct TxstatusHelper {

    // 尝试将状态从 ACTIVE 修改为 ABORTED (Wound-Wait 的核心)
    static bool tryAbort(std::atomic<TxStatus>& status_ref) {
        TxStatus expected = TxStatus::ACTIVE;
        return status_ref.compare_exchange_strong(
            expected,
            TxStatus::ABORTED,
            std::memory_order_acq_rel );
    }

    // 尝试将状态从 ACTIVE 修改为 COMMITTED
    static bool tryCommit(std::atomic<TxStatus>& status_ref) {
        TxStatus expected = TxStatus::ACTIVE;
        return status_ref.compare_exchange_strong(
            expected,
            TxStatus::COMMITTED,
            std::memory_order_acq_rel );
    }

    // 检查是否活跃
    static bool is_active(const std::atomic<TxStatus>& status_ref) {
        return status_ref.load(std::memory_order_acquire) == TxStatus::ACTIVE;
    }
    
    // 检查是否已提交
    static bool is_committed(const std::atomic<TxStatus>& status_ref) {
        return status_ref.load(std::memory_order_acquire) == TxStatus::COMMITTED;
    }
    
    // 检查是否已中止
    static bool is_aborted(const std::atomic<TxStatus>& status_ref) {
        return status_ref.load(std::memory_order_acquire) == TxStatus::ABORTED;
    }
};

}
}