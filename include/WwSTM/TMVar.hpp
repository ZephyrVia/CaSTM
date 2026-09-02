#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <mutex>

#include "Config.hpp"
#include "Logging.hpp"
#include "TaggedPtr.hpp"
#include "VersionNode.hpp"
#include "WriteRecord.hpp"
#include "EBRManager/EBRManager.hpp"
#include "WwSTM/TxDescriptor.hpp"
#include "WwSTM/TxStatus.hpp"

namespace STM {
namespace Ww {

struct TMVarBase {
    virtual ~TMVarBase() = default;

    virtual bool ownsRecord(void* saved_record_ptr, const TxDescriptor* owner) const = 0;
    virtual bool commitReleaseRecord(void* saved_record_ptr, const uint64_t commit_ts) = 0;
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
#if STM_WW_VERIFY_LOGIC_MODE
    mutable std::mutex verify_mu_;
#endif

    template<typename U>
    static inline void retireOrLeak(U* ptr) {
        if (!ptr) return;
#if STM_WW_VERIFY_LOGIC_MODE
        (void)ptr;
#else
        EBRManager::instance()->retire(ptr);
#endif
    }

public:
    template<typename... Args>
    TMVar(Args&&...args) : record_ptr_(nullptr) {
        NodeT* init_node = new NodeT(0, std::forward<Args>(args)...);
        data_ptr_.store(init_node, std::memory_order_release);
    }

    ~TMVar() {
#if STM_WW_VERIFY_LOGIC_MODE
        NodeT* stable = data_ptr_.load(std::memory_order_acquire);
        if (stable) delete stable;
        RecordT* rec = record_ptr_.load(std::memory_order_acquire);
        if (rec) {
            if (rec->new_node) delete rec->new_node;
            delete rec;
        }
#else
        EBRManager::instance()->retire(data_ptr_.load(std::memory_order_acquire));
        RecordT* rec = record_ptr_.load(std::memory_order_acquire);
        if (rec) EBRManager::instance()->retire(rec);
#endif
    }

    // 禁止拷贝和移动
    TMVar(const TMVar&) = delete;
    TMVar& operator=(const TMVar&) = delete;
    TMVar(TMVar&&) = delete;
    TMVar& operator=(TMVar&&) = delete;

    T readProxy(TxDescriptor* tx) {
#if STM_WW_VERIFY_LOGIC_MODE
        std::lock_guard<std::mutex> lk(verify_mu_);
#endif
        RecordT* record = record_ptr_.load(std::memory_order_acquire);

        if(record == nullptr) {
            NodeT* node = data_ptr_.load(std::memory_order_acquire);
            
            if (((uintptr_t)node & 0x3)) {
                std::fprintf(stderr, "[CRITICAL] Var:%p | Corrupt stable node pointer detected: %p\n", (void*)this, (void*)node);
            }
            return node->payload;
        }

        if(record->owner == tx) {
            return record->new_node->payload;
        }

        TxStatus status = record->owner->status.load(std::memory_order_acquire);

        if (status == TxStatus::COMMITTED) {
            return record->new_node->payload;
        } 
        else {
            return record->old_node->payload;
        }
    }

    void* tryWriteAndGetRecord(TxDescriptor* tx, const void* val_ptr, TxDescriptor*& out_conflict) {
        NodeT* my_new_node = new NodeT(tx->start_ts, *static_cast<const T*>(val_ptr));
        RecordT* my_record = new RecordT(tx, nullptr, my_new_node);
#if STM_WW_VERIFY_LOGIC_MODE
        std::lock_guard<std::mutex> lk(verify_mu_);
        RecordT* current = record_ptr_.load(std::memory_order_acquire);
        NodeT* stable_node = data_ptr_.load(std::memory_order_acquire);
        my_record->old_node = stable_node;

        if (current != nullptr) {
            if (current->owner == tx) {
                my_record->old_node = nullptr;
                my_record->new_node = nullptr;
                delete my_record;
                NodeT* old_draft_node = current->new_node;
                current->new_node = my_new_node;
                retireOrLeak(old_draft_node);
                return current;
            }

            TxStatus status = current->owner->status.load(std::memory_order_acquire);
            if (status == TxStatus::ACTIVE || status == TxStatus::COMMITTED) {
                out_conflict = current->owner;
                delete my_new_node;
                delete my_record;
                return nullptr;
            }
            // ABORTED owner can be stolen.
        }

        record_ptr_.store(my_record, std::memory_order_release);
        if (current != nullptr) {
            retireOrLeak(current->new_node);
            retireOrLeak(current);
        }
        return my_record;
#else
        constexpr int kTryWriteRetryLimit = 20000;
        int retry_count = 0;
        while (true) {
            if (++retry_count > kTryWriteRetryLimit) {
                out_conflict = nullptr;
                delete my_new_node;
                delete my_record;
                return nullptr;
            }

            // 击伤检查：本事务已被杀则立刻退出。若 my_record 尚挂在变量上，
            // 保持安装状态（owner=ABORTED），交给后续写者按偷窃协议回收；
            // 未安装则安全删除，避免继续操作一个可能已被判死的事务。
            if (tx->status.load(std::memory_order_acquire) != TxStatus::ACTIVE) {
                out_conflict = nullptr;
                if (record_ptr_.load(std::memory_order_acquire) != my_record) {
                    delete my_new_node;
                    delete my_record;
                }
                return nullptr;
            }

            RecordT* current = record_ptr_.load(std::memory_order_acquire);
            NodeT* stable_node = data_ptr_.load(std::memory_order_acquire);
            
            my_record->old_node = stable_node;

            if(current != nullptr) {
                if (current->owner == tx) {
                    my_record->old_node = nullptr; 
                    my_record->new_node = nullptr; 
                    delete my_record; 

                    NodeT* old_draft_node = current->new_node;
                    current->new_node = my_new_node; 
                    retireOrLeak(old_draft_node);
                    return current;
                }

                TxStatus status = current->owner->status.load(std::memory_order_acquire);

                if(status == TxStatus::ACTIVE) {
                    out_conflict = current->owner;
                    delete my_new_node;
                    delete my_record;
                    return nullptr;
                }
                
                if (status == TxStatus::COMMITTED) {
                    out_conflict = current->owner;
                    delete my_new_node;
                    delete my_record;
                    return nullptr;
                }
            }

            RecordT* expected = current;
            if (record_ptr_.compare_exchange_strong(expected, my_record, std::memory_order_acq_rel)) {
                NodeT* current_data = data_ptr_.load(std::memory_order_acquire);
                if (current_data != stable_node) {
                    // 稳定性失败，卸载重试。必须用 CAS 只卸“仍属于自己”的记录：
                    // 无条件 store 会误清其他线程刚装上的锁。CAS 失败说明本事务
                    // 已被击伤且记录被偷走——对方已按偷窃协议退休本记录，
                    // 所有权已转移，此后绝不能再 delete/复用 my_record。
                    RecordT* mine = my_record;
                    if (record_ptr_.compare_exchange_strong(mine, nullptr, std::memory_order_acq_rel)) {
                        std::this_thread::yield();
                        continue;
                    }
                    out_conflict = nullptr;
                    return nullptr;
                }

                if (current != nullptr) {
                    retireOrLeak(current->new_node);
                    retireOrLeak(current);
                }
                
                return my_record;
            } 
        }
#endif
    }

    bool ownsRecord(void* saved_record_ptr, const TxDescriptor* owner) const override {
        auto* record = static_cast<RecordT*>(saved_record_ptr);
        if (!record || !owner) return false;
        if (record->owner != owner) return false;
        RecordT* current = record_ptr_.load(std::memory_order_acquire);
        return current == record;
    }

    bool commitReleaseRecord(void* saved_record_ptr, const uint64_t commit_ts) override {
#if STM_WW_VERIFY_LOGIC_MODE
        std::lock_guard<std::mutex> lk(verify_mu_);
#endif
        auto* record = static_cast<RecordT*>(saved_record_ptr);
        if (!record) return false;

        RecordT* current = record_ptr_.load(std::memory_order_acquire);
        if (current != record) {
            WWSTM_DLOG("[COMMIT-ERROR] Var:%p | record ownership lost (saved:%p current:%p)\n",
                       (void*)this, (void*)record, (void*)current);
            return false;
        }

        // 验证指针合法性 - 严重错误保留直接输出
        if (((uintptr_t)record->new_node & 0x3)) {
            std::fprintf(stderr, "[CRITICAL-COMMIT] Var:%p | Promoting corrupt NewNode pointer: %p\n", (void*)this, (void*)record->new_node);
        }

        record->new_node->write_ts = commit_ts;
        data_ptr_.store(record->new_node, std::memory_order_release);

        RecordT* expected = record;
        if (!record_ptr_.compare_exchange_strong(expected, nullptr,
                                                 std::memory_order_acq_rel,
                                                 std::memory_order_acquire)) {
            WWSTM_DLOG("[COMMIT-ERROR] Var:%p | failed to release expected record\n", (void*)this);
            return false;
        }

        retireOrLeak(record->old_node);
        retireOrLeak(record);
        return true;
    }

    // 兼容旧测试调用方式：直接提交当前锁记录。
    bool commitReleaseRecord(const uint64_t commit_ts) {
        RecordT* current = record_ptr_.load(std::memory_order_acquire);
        return commitReleaseRecord(static_cast<void*>(current), commit_ts);
    }

    void abortRestoreData(void* saved_record_ptr) override {
        auto* my_record = static_cast<RecordT*>(saved_record_ptr);
#if STM_WW_VERIFY_LOGIC_MODE
        std::lock_guard<std::mutex> lk(verify_mu_);
#endif

        RecordT* expected = my_record;
        if (record_ptr_.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel)) {
            retireOrLeak(my_record->new_node);
            retireOrLeak(my_record);
        }
    }

    uint64_t getDataVersion() const override {
#if STM_WW_VERIFY_LOGIC_MODE
        std::lock_guard<std::mutex> lk(verify_mu_);
#endif
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
