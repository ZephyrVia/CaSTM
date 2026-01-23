#pragma once

#include <atomic>
#include <vector>
#include <thread>
#include <cstdio>
#include <algorithm>

#include "GlobalClock.hpp"
#include "TxDescriptor.hpp"
#include "TxStatus.hpp"
#include "TMVar.hpp"
#include "EBRManager/EBRManager.hpp"

namespace STM {
namespace Ww {

class TxContext {
private:
    struct ReadLogEntry {
        TMVarBase* var;
        uint64_t read_ts;
    };

    struct WriteLogEntry {
        TMVarBase* var;
        void* record_ptr;
    };

    TxDescriptor* my_desc_ = nullptr;
    uint64_t start_ts_ = 0;
    bool is_active_ = false;
    bool in_epoch_ = false;

    std::vector<ReadLogEntry> read_set_;
    std::vector<WriteLogEntry> write_set_;

    size_t get_tid() const {
        return std::hash<std::thread::id>{}(std::this_thread::get_id()) % 1000;
    }

public:
    TxContext(const TxContext&) = delete;
    TxContext& operator=(const TxContext&) = delete;

    TxContext() {
        startNewTransaction();
    }

    ~TxContext() {
        if (my_desc_) {
            if (TxStatusHelper::is_committed(my_desc_->status)) {
                cleanupResources();
            } else {
                abortTransaction();
            }
        } else {
            leaveEpoch();
        }
    }

    void begin() {
        if (my_desc_) {
            abortTransaction();
        }
        startNewTransaction();
    }

    // 辅助函数：允许外部检查事务状态（这对修复 SimpleTree Bug 至关重要）
    bool isActive() const {
        return is_active_;
    }

    bool commit() {
        if (!ensureActive()) return false;

        if (!validateReadSet()) {
            abortTransaction();
            return false;
        }

        if (write_set_.empty()) {
            cleanupResources();
            return true;
        }

        if (!TxStatusHelper::tryCommit(my_desc_->status)) {
            abortTransaction();
            return false;
        }

        uint64_t commit_ts = GlobalClock::tick();
        for (auto& entry : write_set_) {
            entry.var->commitReleaseRecord(commit_ts);
        }

        cleanupResources();
        return true;
    }

    template<typename T>
    T read(TMVar<T>& var) {
        if (!ensureActive()) return T{};

        TMVarBase* var_base = static_cast<TMVarBase*>(&var);
        for (auto& entry : read_set_) {
            if (entry.var == var_base) {
                return var.readProxy(my_desc_);
            }
        }

        uint64_t v_pre = var.getDataVersion();
        T val = var.readProxy(my_desc_);
        uint64_t v_post = var.getDataVersion();

        if (v_pre != v_post) {
            abortTransaction();
            return T{};
        }

        read_set_.push_back({var_base, v_pre});
        return val;
    }

    template<typename T>
    void write(TMVar<T>& var, const T& val) {
        size_t tid = get_tid();
        if (!ensureActive()) {
            // std::printf("[T%zu] [WRITE-SKIP] Tx inactive, skipping write\n", tid);
            return;
        }

        TMVarBase* var_base = static_cast<TMVarBase*>(&var);
        
        // 1. 重入检查：如果已经持有锁，直接更新
        for (const auto& entry : write_set_) {
            if (entry.var == var_base) {
                TxDescriptor* dummy = nullptr;
                var.tryWriteAndGetRecord(my_desc_, &val, dummy);
                return;
            }
        }

        // 2. 尝试获取锁
        while (true) {
            TxDescriptor* conflict_tx = nullptr;
            void* record = var.tryWriteAndGetRecord(my_desc_, &val, conflict_tx);

            if (record) {
                // 【核心修复】获取锁后再次验证版本，防止 Lost Update
                bool found_in_readset = false;
                for (const auto& r_entry : read_set_) {
                    if (r_entry.var == var_base) {
                        found_in_readset = true;
                        if (var.getDataVersion() != r_entry.read_ts) {
                            std::printf("[T%zu] [WRITE-ABORT] Stale Lock! ReadVer:%lu != CurrVer:%lu\n", 
                                        tid, r_entry.read_ts, var.getDataVersion());
                            var.abortRestoreData(record); // 立即释放锁
                            abortTransaction();
                            return;
                        }
                        break;
                    }
                }
                
                // 如果是"盲写"（不在读集中），在你的树算法中是不应该发生的
                // 这里我们暂且允许，但记录下来
                
                trackWrite(var_base, record);
                return;
            }

            resolveConflict(conflict_tx);

            if (!ensureActive()) return;
            std::this_thread::yield();
        }
    }

private:
    void startNewTransaction() {
        enterEpoch();
        read_set_.clear();
        write_set_.clear();
        start_ts_ = GlobalClock::now();
        my_desc_ = new TxDescriptor(start_ts_);
        my_desc_->status.store(TxStatus::ACTIVE, std::memory_order_release);
        is_active_ = true;
    }

    void abortTransaction() {
        if (!my_desc_) return;
        TxStatusHelper::tryAbort(my_desc_->status);
        is_active_ = false;
        for (auto it = write_set_.rbegin(); it != write_set_.rend(); ++it) {
            it->var->abortRestoreData(it->record_ptr);
        }
        cleanupResources();
    }

    void cleanupResources() {
        read_set_.clear();
        write_set_.clear();
        is_active_ = false;
        if (my_desc_) {
            // EBRManager::instance()->retire(my_desc_);
            my_desc_ = nullptr;
        }
        leaveEpoch();
    }

    bool ensureActive() {
        if (!is_active_) return false;
        if (!my_desc_) return false;
        if (my_desc_->status.load(std::memory_order_relaxed) == TxStatus::ABORTED) {
            is_active_ = false;
        }
        return is_active_;
    }

    void trackWrite(TMVarBase* var, void* record) {
        write_set_.push_back({var, record});
    }

    bool validateReadSet() {
        for (const auto& entry : read_set_) {
            bool locked_by_me = false;
            for (const auto& w_entry : write_set_) {
                if (w_entry.var == entry.var) {
                    locked_by_me = true;
                    break;
                }
            }
            if (locked_by_me) continue;
            if (entry.var->getDataVersion() != entry.read_ts) return false;
        }
        return true;
    }

    void enterEpoch() {
        if (!in_epoch_) {
            EBRManager::instance()->enter();
            in_epoch_ = true;
        }
    }

    void leaveEpoch() {
        if (in_epoch_) {
            EBRManager::instance()->leave();
            in_epoch_ = false;
        }
    }

    void resolveConflict(TxDescriptor* conflict_tx) {
        if (!conflict_tx) return;
        TxStatus s = conflict_tx->status.load(std::memory_order_acquire);
        if (s == TxStatus::ABORTED) return;
        if (s == TxStatus::COMMITTED) {
            while (conflict_tx->status.load(std::memory_order_acquire) == TxStatus::COMMITTED) {
                std::this_thread::yield();
            }
            return;
        }
        uint64_t my_ts = start_ts_;
        uint64_t enemy_ts = conflict_tx->start_ts;
        bool i_am_older = (my_ts < enemy_ts);
        if (my_ts == enemy_ts) i_am_older = (my_desc_ < conflict_tx);

        if (i_am_older) {
            if (TxStatusHelper::tryAbort(conflict_tx->status)) return;
            else {
                s = conflict_tx->status.load(std::memory_order_acquire);
                if (s == TxStatus::COMMITTED) std::this_thread::yield();
            }
        } else {
            abortTransaction();
        }
    }
};

}
}