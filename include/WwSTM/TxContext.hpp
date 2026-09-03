#pragma once

#include <atomic>
#include <vector>
#include <thread>
#include <chrono>
#include <algorithm>
#include <mutex>

#include "Config.hpp"
#include "Logging.hpp"
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
        const void* node;
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
    std::vector<void*> allocated_ptrs_; 

#if STM_WW_TEST_HOOKS
    using PrepareHook = void (*)(TxContext&);
    using CommittedHook = void (*)(TxContext&);
    inline static std::atomic<PrepareHook> prepare_hook_{nullptr};
    inline static std::atomic<CommittedHook> committed_hook_{nullptr};
#endif

public:
    TxContext(const TxContext&) = delete;
    TxContext& operator=(const TxContext&) = delete;

    TxContext() {
        startNewTransaction();
    }

#if STM_WW_TEST_HOOKS
    static void setPrepareHook(PrepareHook hook) noexcept {
        prepare_hook_.store(hook, std::memory_order_release);
    }

    static void clearPrepareHook() noexcept {
        setPrepareHook(nullptr);
    }

    static void setCommittedHook(CommittedHook hook) noexcept {
        committed_hook_.store(hook, std::memory_order_release);
    }

    static void clearCommittedHook() noexcept {
        setCommittedHook(nullptr);
    }

    TxDescriptor* debugDescriptorForTest() const noexcept {
        return my_desc_;
    }
#endif

    ~TxContext() {
        if (my_desc_) {
            if (TxStatusHelper::is_committed(my_desc_->status)) {
                // COMMITTED is irreversible, but a helper may still have to
                // flatten one of this transaction's published records.  Do
                // that before retiring the descriptor.
                finishCommitted_();
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

    bool isActive() const {
        if (!is_active_ || !my_desc_) return false;
        return my_desc_->status.load(std::memory_order_acquire) == TxStatus::ACTIVE;
    }

    bool commit() {
        if (!ensureActive()) return false;

        // Freeze owner writes for the complete validate/prepare/status-CAS
        // sequence.  TMVar::tryWriteAndGetRecord takes the same gate, so a
        // concurrent self-write cannot mutate a draft after preparation.
        std::unique_lock<std::mutex> prepare_lock(my_desc_->write_gate);
        TxStatus observed_status = my_desc_->status.load(std::memory_order_acquire);
        if (observed_status != TxStatus::ACTIVE) {
            prepare_lock.unlock();
            if (observed_status == TxStatus::COMMITTED) {
                finishCommitted_();
                return true;
            }
            abortTransaction();
            return false;
        }
        my_desc_->prepare_started.store(true, std::memory_order_release);

        if (!validateReadSet()) {
            prepare_lock.unlock();
            abortTransaction();
            return false;
        }

        if (!validateWriteSetOwnership()) {
            prepare_lock.unlock();
            abortTransaction();
            return false;
        }

        const uint64_t commit_ts = write_set_.empty() ? 0 : GlobalClock::tick();
        for (auto& entry : write_set_) {
            if (!entry.var->prepareCommit(entry.record_ptr,
                                          my_desc_,
                                          commit_ts)) {
                // No record has been logically committed yet.  The descriptor
                // remains ACTIVE or has already been wounded, so abort can
                // restore every prepared/installed generation.
                prepare_lock.unlock();
                abortTransaction();
                return false;
            }
        }

#if STM_WW_TEST_HOOKS
        if (!write_set_.empty()) {
            if (auto hook = prepare_hook_.load(std::memory_order_acquire)) {
                hook(*this);
            }
        }
#endif

        // The single transaction-level linearization point.  Every prepared
        // WriteRecord now has its final payload and timestamp; after this CAS
        // no path may turn the transaction into ABORTED.
        if (!TxStatusHelper::tryCommit(my_desc_->status)) {
            observed_status = my_desc_->status.load(std::memory_order_acquire);
            prepare_lock.unlock();
            if (observed_status == TxStatus::COMMITTED) {
                // Another commit attempt (or a deterministic test hook) may
                // have crossed the same irreversible point.  The logical
                // result is still success; only physical cleanup remains.
                finishCommitted_();
                return true;
            }
            abortTransaction();
            return false;
        }

        // The mutex is a preparation gate only.  Release it before cleanup so
        // its containing descriptor cannot be reclaimed while this function
        // is still implicitly unlocking the mutex at scope exit.
        prepare_lock.unlock();

#if STM_WW_TEST_HOOKS
        if (!write_set_.empty()) {
            if (auto hook = committed_hook_.load(std::memory_order_acquire)) {
                hook(*this);
            }
        }
#endif

        finishCommitted_();
        return true;
    }

    template<typename T, typename... Args>
    TMVar<T>* alloc(Args&&... args) {
#if STM_WW_VERIFY_LOGIC_MODE
        auto* obj = new TMVar<T>(std::forward<Args>(args)...);
        recordAllocation(static_cast<void*>(obj));
        WWSTM_DLOG("[TxAlloc] Addr=%p\n", (void*)obj);
        return obj;
#else
        void* raw_mem = ThreadHeap::allocate(sizeof(TMVar<T>));
        recordAllocation(raw_mem);
        WWSTM_DLOG("[TxAlloc] Addr=%p\n", raw_mem);

        return new(raw_mem) TMVar<T>(std::forward<Args>(args)...);
#endif
    }

    template<typename T>
    T read(TMVar<T>* var_ptr) {
        if (!var_ptr) {
            return T{}; 
        }
        if (!ensureActive()) return T{};

        TMVar<T>& var = *var_ptr;
        TMVarBase* var_base = static_cast<TMVarBase*>(&var);

        // 1. 检查写集 (Read-your-own-writes).  The concrete Record type is
        // deliberately hidden behind TMVar::readSnapshot().
        for (const auto& entry : write_set_) {
            if (entry.var == var_base) {
                return var.readSnapshot(my_desc_).value;
            }
        }

        // 2. 检查读集 (重复读快照校验)
        // 重复读必须仍处于首次读的版本，否则按冲突 abort。
        // 不更新 read_ts，同一事务已基于旧版本建立快照。
        for (auto& entry : read_set_) {
            if (entry.var == var_base) {
                auto snapshot = var.readSnapshot(my_desc_);
                if (snapshot.node != entry.node || snapshot.version != entry.read_ts) {
                    abortTransaction();
                    return T{};
                }
                return snapshot.value;
            }
        }

        auto snapshot = var.readSnapshot(my_desc_);
        read_set_.push_back({var_base, snapshot.version, snapshot.node});
        return snapshot.value;
    }

    template<typename T>
    void write(TMVar<T>* var_ptr, const T& val) {
        if (!var_ptr) return;

        // size_t tid = get_tid(); // 仅在需要日志时使用
        if (!ensureActive()) return;

        TMVar<T>& var = *var_ptr;
        TMVarBase* var_base = static_cast<TMVarBase*>(&var);
        
        // 1. 重入检查
        for (const auto& entry : write_set_) {
            if (entry.var == var_base) {
                TxDescriptor* dummy = nullptr;
                var.tryWriteAndGetRecord(my_desc_, &val, dummy);
                return;
            }
        }

        // 2. 尝试获取锁
#if STM_WW_VERIFY_LOGIC_MODE
        constexpr int kWriteRetryLimit = 1000000;
#else
        constexpr int kWriteRetryLimit = 10000;
#endif
        int retry_count = 0;
        while (true) {
            if (++retry_count > kWriteRetryLimit) {
                abortTransaction();
                return;
            }

            TxDescriptor* conflict_tx = nullptr;
            void* record = var.tryWriteAndGetRecord(my_desc_, &val, conflict_tx);

            if (record) {
                // 获取锁后再次验证版本
                for (const auto& r_entry : read_set_) {
                    if (r_entry.var == var_base) {
                        if (!var.validateReadBeforeWrite(record,
                                                         r_entry.node,
                                                         r_entry.read_ts)) {
                            var.abortRestoreData(record); 
                            abortTransaction();
                            return;
                        }
                        break;
                    }
                }
                
                trackWrite(var_base, record);
                return;
            }

            resolveConflict(conflict_tx);

            if (!ensureActive()) return;
            if (my_desc_->prepare_started.load(std::memory_order_acquire)) return;
            if ((retry_count & 0x3F) == 0) {
                std::this_thread::sleep_for(std::chrono::microseconds(1));
            } else {
                std::this_thread::yield();
            }
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
        TxStatus observed = my_desc_->status.load(std::memory_order_acquire);
        if (observed == TxStatus::COMMITTED) {
            // COMMITTED is irreversible.  A failed status CAS, a destructor,
            // or any late cleanup path must never restore an old value after
            // the transaction has crossed its logical commit point.
            is_active_ = false;
            finishCommitted_();
            return;
        }

        TxStatusHelper::tryAbort(my_desc_->status);
        observed = my_desc_->status.load(std::memory_order_acquire);
        if (observed == TxStatus::COMMITTED) {
            // A concurrent status transition won the race while this caller
            // was trying to abort.  The committed side owns cleanup.
            is_active_ = false;
            finishCommitted_();
            return;
        }

        is_active_ = false;
        for (auto it = write_set_.rbegin(); it != write_set_.rend(); ++it) {
            it->var->abortRestoreData(it->record_ptr);
        }

        for (void* ptr : allocated_ptrs_) {
#if STM_WW_VERIFY_LOGIC_MODE
            delete static_cast<TMVarBase*>(ptr);
#else
            ThreadHeap::deallocate(ptr);
#endif
        }
        cleanupResources();
    }

    void finishCommitted_() {
        // Physical cleanup is deliberately infallible.  A helper may have
        // flattened any token already; that only changes where cleanup is
        // performed, never the committed result.
        for (const auto& entry : write_set_) {
            entry.var->helpComplete(entry.record_ptr);
        }
        cleanupResources();
    }

    static void retireDescriptor_(TxDescriptor* descriptor) {
        if (!descriptor) return;
#if STM_WW_VERIFY_LOGIC_MODE
        // VERIFY mode deliberately has no physical EBR reclamation: published
        // records are retained so logic tests can keep raw white-box tokens.
        (void)descriptor;
#else
        EBRManager::instance()->retire(descriptor);
#endif
    }

    void cleanupResources() {
        read_set_.clear();
        write_set_.clear();
        allocated_ptrs_.clear();

        is_active_ = false;
        TxDescriptor* finished_descriptor = my_desc_;
        if (my_desc_) {
            my_desc_ = nullptr;
        }
        // All records have already been removed/retired by owner or helper.
        // Retire the descriptor after that point, before leaving this EBR
        // section, so readers that loaded a record remain protected.
        retireDescriptor_(finished_descriptor);
        leaveEpoch();
    }

    bool ensureActive() {
        if (!is_active_) return false;
        if (!my_desc_) return false;
        if (my_desc_->status.load(std::memory_order_acquire) != TxStatus::ACTIVE) {
            is_active_ = false;
        }
        return is_active_;
    }

    void trackWrite(TMVarBase* var, void* record) {
        write_set_.push_back({var, record});
    }

    void recordAllocation(void* ptr) {
        allocated_ptrs_.push_back(ptr);
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
            if (!entry.var->validateReadSnapshot(entry.node,
                                                 entry.read_ts,
                                                 my_desc_)) {
                return false;
            }
        }
        return true;
    }

    bool validateWriteSetOwnership() {
        for (const auto& entry : write_set_) {
            if (!entry.var->validateForCommit(entry.record_ptr, my_desc_)) {
                return false;
            }
        }
        return true;
    }

    void enterEpoch() {
#if STM_WW_VERIFY_LOGIC_MODE
        in_epoch_ = true;
#else
        if (!in_epoch_) {
            EBRManager::instance()->enter();
            in_epoch_ = true;
        }
#endif
    }

    void leaveEpoch() {
#if STM_WW_VERIFY_LOGIC_MODE
        in_epoch_ = false;
#else
        if (in_epoch_) {
            EBRManager::instance()->leave();
            in_epoch_ = false;
        }
#endif
    }

    void resolveConflict(TxDescriptor* conflict_tx) {
        if (!conflict_tx) return;
        TxStatus s = conflict_tx->status.load(std::memory_order_acquire);
        if (s == TxStatus::ABORTED) return;
        if (s == TxStatus::COMMITTED) {
            // COMMITTED 是终态，不应等待其“离开 COMMITTED”。
            // 直接返回并让上层重试获取写锁即可。
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
