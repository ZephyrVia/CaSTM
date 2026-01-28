#pragma once

#include <atomic>
#include <cstdint>
#include <type_traits>
#include <thread>
#include <functional> 
#include <cstdio> // 必须包含用于 printf

#include "TaggedPtr.hpp"
#include "VersionNode.hpp"
#include "WriteRecord.hpp"
#include "EBRManager/EBRManager.hpp"
#include "WwSTM/TxDescriptor.hpp"
#include "WwSTM/TxStatus.hpp"

// =============================================================
// 日志控制开关
// 0: 关闭日志 (高性能，生产环境)
// 1: 开启日志 (调试环境，可能会导致 I/O 瓶颈)
// =============================================================
#ifndef STM_ENABLE_LOGGING
#define STM_ENABLE_LOGGING1 0
#endif

#if STM_ENABLE_LOGGING
    #define STM_LOG(...) std::fprintf(stderr, __VA_ARGS__)
#else
    #define STM_LOG(...) ((void)0)
#endif

namespace STM {
namespace Ww {

// 为了让 TxContext::write_set_ 可以通过 std::vector<TMVarBase*> 统一存储不同类型的变量
struct TMVarBase {
    virtual ~TMVarBase() = default;

    virtual void commitReleaseRecord(const uint64_t commit_ts) = 0;
    virtual void abortRestoreData(void* saved_record_ptr) = 0;
    virtual uint64_t getDataVersion() const = 0;
};


template <typename T>
class TMVar : public TMVarBase {
public:
    using NodeT = detail::VersionNode<T>;
    using RecordT = detail::WriteRecord<T>;

private:
    std::atomic<NodeT*> data_ptr_;
    std::atomic<RecordT*> record_ptr_;

    // 日志辅助：获取短线程ID
    size_t get_tid() const {
        return std::hash<std::thread::id>{}(std::this_thread::get_id()) % 1000;
    }

public:
    template<typename... Args>
    TMVar(Args&&...args) : record_ptr_(nullptr) {
        NodeT* init_node = new NodeT(0, std::forward<Args>(args)...);
        data_ptr_.store(init_node, std::memory_order_release);
        STM_LOG("[T%zu] [CONSTRUCT] Var:%p | InitDataNode:%p | Initialized\n", get_tid(), (void*)this, (void*)init_node);
    }

    ~TMVar() {
        STM_LOG("[T%zu] [DESTRUCT] Var:%p | Destroying TMVar\n", get_tid(), (void*)this);
        EBRManager::instance()->retire(data_ptr_.load(std::memory_order_acquire));
        RecordT* rec = record_ptr_.load(std::memory_order_acquire);
        if (rec) EBRManager::instance()->retire(rec);
    }

    // 禁止拷贝和移动
    TMVar(const TMVar&) = delete;
    TMVar& operator=(const TMVar&) = delete;
    TMVar(TMVar&&) = delete;
    TMVar& operator=(TMVar&&) = delete;

    T readProxy(TxDescriptor* tx) {
        // size_t tid = get_tid(); // 仅在启用日志时需要，避免未使用变量警告
        RecordT* record = record_ptr_.load(std::memory_order_acquire);

        // Case 1: 无锁 -> 直接读
        if(record == nullptr) {
            NodeT* node = data_ptr_.load(std::memory_order_acquire);
            STM_LOG("[T%zu] [READ-STABLE] Var:%p | Node:%p | ValAddr:%p\n", get_tid(), (void*)this, (void*)node, (void*)&node->payload);
            
            // 关键错误检查保留直接输出
            if (((uintptr_t)node & 0x3)) {
                std::fprintf(stderr, "[CRITICAL] Var:%p | Corrupt stable node pointer detected: %p\n", (void*)this, (void*)node);
            }
            return node->payload;
        }

        // Case 2: 有锁 -> 检查 Owner
        if(record->owner == tx) {
            STM_LOG("[T%zu] [READ-OWNER] Var:%p | TxDesc:%p | Reading my own NewNode:%p\n", get_tid(), (void*)this, (void*)tx, (void*)record->new_node);
            return record->new_node->payload;
        }

        // Case 3: 冲突检测 (Wound-Wait 读策略)
        TxStatus status = record->owner->status.load(std::memory_order_acquire);

        if (status == TxStatus::COMMITTED) {
            STM_LOG("[T%zu] [READ-SNAPSHOT] Var:%p | Owner:%p (COMMITTED) | Reading NewNode:%p\n", get_tid(), (void*)this, (void*)record->owner, (void*)record->new_node);
            return record->new_node->payload;
        } 
        else {
            STM_LOG("[T%zu] [READ-SNAPSHOT] Var:%p | Owner:%p (ACTIVE/ABORT) | Reading OldNode:%p\n", get_tid(), (void*)this, (void*)record->owner, (void*)record->old_node);
            return record->old_node->payload;
        }
    }

    void* tryWriteAndGetRecord(TxDescriptor* tx, const void* val_ptr, TxDescriptor*& out_conflict) {
        size_t tid = get_tid();
        
        NodeT* my_new_node = new NodeT(tx->start_ts, *static_cast<const T*>(val_ptr));
        RecordT* my_record = new RecordT(tx, nullptr, my_new_node);

        STM_LOG("[T%zu] [WRITE-INIT] Var:%p | NewNode:%p | Record:%p | StartTS:%lu\n", tid, (void*)this, (void*)my_new_node, (void*)my_record, tx->start_ts);

        while (true) {
            RecordT* current = record_ptr_.load(std::memory_order_acquire);
            NodeT* stable_node = data_ptr_.load(std::memory_order_acquire);
            
            my_record->old_node = stable_node;

            if(current != nullptr) {
                // --- 重入 (Re-entrant) ---
                if (current->owner == tx) {
                    STM_LOG("[T%zu] [WRITE-REENTRANT] Var:%p | Owner:%p | Replacing DraftNode %p -> %p\n", tid, (void*)this, (void*)tx, (void*)current->new_node, (void*)my_new_node);
                    
                    my_record->old_node = nullptr; 
                    my_record->new_node = nullptr; 
                    delete my_record; 

                    NodeT* old_draft_node = current->new_node;
                    current->new_node = my_new_node; 
                    EBRManager::instance()->retire(old_draft_node); 
                    return current;
                }

                TxStatus status = current->owner->status.load(std::memory_order_acquire);

                // --- 冲突 (Active) ---
                if(status == TxStatus::ACTIVE) {
                    STM_LOG("[T%zu] [WRITE-CONFLICT] Var:%p | Owner:%p is ACTIVE | Failing\n", tid, (void*)this, (void*)current->owner);
                    out_conflict = current->owner;
                    delete my_new_node;
                    delete my_record;
                    return nullptr;
                }
                
                // --- 冲突 (Committed but not cleaned) ---
                if (status == TxStatus::COMMITTED) {
                    STM_LOG("[T%zu] [WRITE-WAIT] Var:%p | Owner:%p is COMMITTED | Yielding\n", tid, (void*)this, (void*)current->owner);
                    std::this_thread::yield();
                    continue; 
                }

                // --- 抢占 (Steal Aborted) ---
                STM_LOG("[T%zu] [WRITE-STEAL] Var:%p | Owner:%p is ABORTED | Stealing lock\n", tid, (void*)this, (void*)current->owner);
            }

            // --- CAS 尝试上位 ---
            RecordT* expected = current;
            if (record_ptr_.compare_exchange_strong(expected, my_record, std::memory_order_acq_rel)) {
                // 【核心修复】ABA 检查
                NodeT* current_data = data_ptr_.load(std::memory_order_acquire);
                if (current_data != stable_node) {
                    STM_LOG("[T%zu] [WRITE-ABA] Var:%p | Stale Data! Stable:%p != Curr:%p | Backing off\n", 
                        tid, (void*)this, (void*)stable_node, (void*)current_data);
                    
                    // 回滚：释放刚才抢到的锁
                    record_ptr_.store(nullptr, std::memory_order_release);
                    
                    // 重新尝试循环
                    std::this_thread::yield();
                    continue; 
                }

                if (current != nullptr) {
                    EBRManager::instance()->retire(current->new_node);
                    EBRManager::instance()->retire(current);
                }
                
                return my_record;
            } 
            else {
                STM_LOG("[T%zu] [WRITE-RETRY] Var:%p | CAS failed, someone else updated record_ptr_\n", tid, (void*)this);
            }
        }
    }

    void commitReleaseRecord(const uint64_t commit_ts) override {
        RecordT* record = record_ptr_.load(std::memory_order_acquire);
        
        if (!record) {
            STM_LOG("[T%zu] [COMMIT-ERROR] Var:%p | record_ptr_ is NULL during commit!\n", get_tid(), (void*)this);
            return; 
        }

        STM_LOG("[T%zu] [COMMIT-START] Var:%p | Promoting NewNode:%p to Stable | CommitTS:%lu\n", get_tid(), (void*)this, (void*)record->new_node, commit_ts);

        // 验证指针合法性 - 严重错误保留直接输出
        if (((uintptr_t)record->new_node & 0x3)) {
            std::fprintf(stderr, "[CRITICAL-COMMIT] Var:%p | Promoting corrupt NewNode pointer: %p\n", (void*)this, (void*)record->new_node);
        }

        record->new_node->write_ts = commit_ts;
        data_ptr_.store(record->new_node, std::memory_order_release);
        record_ptr_.store(nullptr, std::memory_order_release);

        STM_LOG("[T%zu] [COMMIT-SUCCESS] Var:%p | Lock released, data_ptr_ updated\n", get_tid(), (void*)this);

        EBRManager::instance()->retire(record->old_node);
        EBRManager::instance()->retire(record);
    }

    void abortRestoreData(void* saved_record_ptr) override {
        size_t tid = get_tid();
        auto* my_record = static_cast<RecordT*>(saved_record_ptr);
        
        STM_LOG("[T%zu] [ABORT-START] Var:%p | Attempting to rollback Record:%p\n", tid, (void*)this, (void*)my_record);

        RecordT* expected = my_record;
        if (record_ptr_.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel)) {
            STM_LOG("[T%zu] [ABORT-CLEAN] Var:%p | Rollback success, lock cleared\n", tid, (void*)this);
            EBRManager::instance()->retire(my_record->new_node);
            EBRManager::instance()->retire(my_record);
        } 
        else {
            STM_LOG("[T%zu] [ABORT-STOLEN] Var:%p | Lock was already stolen by Record:%p\n", tid, (void*)this, (void*)expected);
        }
    }

    uint64_t getDataVersion() const override {
        // 严重错误检查 - 保留 std::fprintf(stderr, ...)
        if (reinterpret_cast<uintptr_t>(this) < 4096) {
            std::fprintf(stderr, "[FATAL] TMVar 'this' is invalid! Addr: %p\n", (void*)this);
            std::abort();
        }

        NodeT* node = data_ptr_.load(std::memory_order_acquire);
        
        if (node == nullptr) {
            std::fprintf(stderr, "[FATAL] Var:%p | data_ptr_ is NULL!\n", (void*)this);
            std::abort();
        }

        if (((uintptr_t)node & 0x3)) {
            std::fprintf(stderr, "[FATAL] Var:%p | Corrupt node pointer in getDataVersion: %p\n", (void*)this, (void*)node);
            // 这里不立刻 abort，让日志输完
        }

        return node->write_ts; 
    }
};

} // namespace Ww
} // namespace STM