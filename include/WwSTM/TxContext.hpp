#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>
#include <thread>

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
    struct WriteLogEntry {
        TMVarBase* var;
        void* record_ptr;
    };

    TxDescriptor* my_desc_ = nullptr;
    uint64_t start_ts_ = 0;
    bool is_valid_ = true;      // 事务描述符是否有效
    bool in_epoch_ = false;     // 事务是否进入纪元，被EBRManager所管理

    std::vector<WriteLogEntry> write_set_;

public:
    TxContext() {
        begin();
    }

    ~TxContext() {
        // 1. 先检查描述符是否存在
        if (my_desc_) {
            if (!TxStatusHelper::is_committed(my_desc_->status)) {
                abortImpl();
            }
            else {
                // 防御性编程：如果是 Committed 但没退休（极其罕见），退休它
                retireDescriptor();
                leaveEpoch();
            }
        } 
        else {
            leaveEpoch();
        }
    }


    void begin() {
        enterEpoch();

        write_set_.clear();
        is_valid_ = true;

        start_ts_ = GlobalClock::now();

        my_desc_ = new TxDescriptor(start_ts_);
        my_desc_->status.store(TxStatus::ACTIVE, std::memory_order_release);
    }

    bool commit() {
        if (!checkValidity()) {
            abortImpl();
            return false;
        }

        if (write_set_.empty()){
            retireDescriptor();
            leaveEpoch();
            return true;
        }

        if (!TxStatusHelper::tryCommit(my_desc_->status)) {
            // 切换失败，说明被高优先级事务 Wound (Kill) 了
            abortImpl();
            return false;
        }

        uint64_t commit_ts = GlobalClock::tick();

        for (auto& entry : write_set_) {
            entry.var->commitReleaseRecord(commit_ts);
        }

        write_set_.clear();
        retireDescriptor();
        leaveEpoch();

        return true;
    }

    template<typename T>
    const T& read(TMVar<T>& var) {
        if(!checkValidity()) {
            static T dummy{};
            return dummy;
        }

        return var.readProxy(my_desc_);
    }

    template<typename T> 
    void write(TMVar<T>& var, const T& val) {
        if (!checkValidity()) 
            return;

        while (true) {
            TxDescriptor* conflict_tx = nullptr;

            void* record = var.tryWriteAndGetRecord(my_desc_, &val, conflict_tx);
            if(record) {
                write_set_.push_back({&var, record});
                var.installNewNode(record);
                return;
            }
            resolveConflict(conflict_tx);

            if(!checkValidity()) {
                return;
            }

            std::this_thread::yield();
        }
    }


private:
    bool checkValidity() {
        if(!is_valid_) return false;
        if(!my_desc_) return false; 

        if(my_desc_->status.load(std::memory_order_relaxed) == TxStatus::ABORTED) {
            is_valid_ = false;
        }
        return is_valid_;
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

    void abortImpl() {
        if (!my_desc_) {
            is_valid_ = false;
            write_set_.clear();
            leaveEpoch();
            return;
        }

        TxStatusHelper::tryAbort(my_desc_->status);
        is_valid_ = false;

        for (auto it = write_set_.rbegin(); it != write_set_.rend(); ++it) {
            it->var->abortRestoreData(it->record_ptr);
        }
        write_set_.clear();
        retireDescriptor();
        leaveEpoch();
    }

    void retireDescriptor() {
        if (my_desc_) {
            EBRManager::instance()->retire(my_desc_);
            my_desc_ = nullptr; // 防止悬垂指针
        }
    }

    void resolveConflict(TxDescriptor* conflict_tx) {
        if(!conflict_tx) return;

        uint64_t my_ts = start_ts_;
        uint64_t enemy_ts = conflict_tx->start_ts;

        bool i_am_older = false;

        if(my_ts != enemy_ts) {
            // 时间戳越小越老
            i_am_older = (my_ts < enemy_ts);
        } 
        else {
            // Tie-Breaker: 时间戳相同比较地址，保证全序
            i_am_older = (my_desc_ < conflict_tx);
        }

        if(i_am_older) {
            // Case A: 我是老资格 (Wound) -> 杀掉对方
            if(TxStatusHelper::tryAbort(conflict_tx->status)) {
                return;
            } 
            else {
                if (TxStatusHelper::is_aborted(conflict_tx->status)) {
                    return;
                }
                if (TxStatusHelper::is_committed(conflict_tx->status)) {
                    // 为了避免 CPU 空转 (Spinning)，这里 yield 一下
                    std::this_thread::yield();
                    return;
                }
            }

        }
        else {
            TxStatus s = conflict_tx->status.load(std::memory_order_acquire);
            if (s == TxStatus::ABORTED || s == TxStatus::COMMITTED) {
                return; // 捡回一条命，重试
            }

            abortImpl();
        }
    }


};


}
}