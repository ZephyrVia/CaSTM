#pragma once

#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>

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

template<typename T>
struct ReadSnapshot {
    using NodeT = detail::VersionNode<T>;

    T value;
    uint64_t version;
    const NodeT* node;
};

struct TMVarBase {
    virtual ~TMVarBase() = default;

    virtual bool ownsRecord(void* saved_record_ptr, const TxDescriptor* owner) const = 0;
    virtual bool commitReleaseRecord(void* saved_record_ptr, uint64_t commit_ts) = 0;
    virtual void abortRestoreData(void* saved_record_ptr) = 0;
    virtual bool validateReadSnapshot(const void* node_ptr,
                                      uint64_t version,
                                      const TxDescriptor* tx) const = 0;
    virtual bool validateReadBeforeWrite(void* record_ptr,
                                         const void* node_ptr,
                                         uint64_t version) const = 0;
    virtual uint64_t getDataVersion() const = 0;
};

template <typename T>
class TMVar : public TMVarBase {
public:
    using NodeT = detail::VersionNode<T>;
    using RecordT = detail::WriteRecord<T>;
    using HeadWord = uintptr_t;

#if STM_WW_TEST_HOOKS
    using ReadSnapshotHook = void (*)(TMVar<T>&);
#endif

private:
    // The only shared state entry.  Bit 0 == 0 means VersionNode, bit 0 == 1
    // means WriteRecord.  Both types are system-heap allocated and have a
    // free low bit, which is checked below.
    std::atomic<HeadWord> head_;

#if STM_WW_TEST_HOOKS
    inline static std::atomic<ReadSnapshotHook> read_snapshot_hook_{nullptr};
#endif

#if STM_WW_VERIFY_LOGIC_MODE
    mutable std::mutex verify_mu_;
#endif

    static_assert(alignof(NodeT) >= 2,
                  "VersionNode must have a free low bit for head tagging");
    static_assert(alignof(RecordT) >= 2,
                  "WriteRecord must have a free low bit for head tagging");

    static HeadWord packNode_(NodeT* node) noexcept {
        assert(node != nullptr);
        return TaggedPtrHelper::packNode(node);
    }

    static HeadWord packRecord_(RecordT* record) noexcept {
        assert(record != nullptr);
        return TaggedPtrHelper::packRecord(record);
    }

    static NodeT* unpackNode_(HeadWord raw) noexcept {
        if (raw == 0 || !TaggedPtrHelper::isNode(raw)) return nullptr;
        return TaggedPtrHelper::unpackNode<NodeT>(raw);
    }

    static RecordT* unpackRecord_(HeadWord raw) noexcept {
        if (raw == 0 || !TaggedPtrHelper::isRecord(raw)) return nullptr;
        return TaggedPtrHelper::unpackRecord<RecordT>(raw);
    }

    template<typename U>
    static void retireOrLeak(U* ptr) {
        if (!ptr) return;
#if STM_WW_VERIFY_LOGIC_MODE
        // Verification mode intentionally keeps published objects alive.
        // TxContext may retain a write-set token until its destructor, while
        // another thread can already have flattened the corresponding head.
        (void)ptr;
#else
        EBRManager::instance()->retire(ptr);
#endif
    }

    static NodeT* selectVisibleNode_(HeadWord raw,
                                     const TxDescriptor* tx) noexcept {
        if (NodeT* node = unpackNode_(raw)) return node;

        RecordT* record = unpackRecord_(raw);
        if (!record || !record->owner) return nullptr;

        TxStatus status = record->owner->status.load(std::memory_order_acquire);
        if (status == TxStatus::COMMITTED ||
            (record->owner == tx && status == TxStatus::ACTIVE)) {
            return record->new_node;
        }

        // A foreign ACTIVE record and any ABORTED record expose old_node.
        return record->old_node;
    }

    NodeT* loadVisibleNode_(const TxDescriptor* tx) const noexcept {
        for (;;) {
            HeadWord h1 = head_.load(std::memory_order_acquire);
            NodeT* selected = selectVisibleNode_(h1, tx);
            if (selected == nullptr) {
                std::fprintf(stderr, "[FATAL] Var:%p | invalid head state\n",
                             static_cast<const void*>(this));
                std::abort();
            }

            HeadWord h2 = head_.load(std::memory_order_acquire);
            if (h1 == h2) return selected;
            std::this_thread::yield();
        }
    }

    bool helpResolveRecord_(RecordT* record, TxStatus terminal_status) noexcept {
        if (!record || !record->owner ||
            (terminal_status != TxStatus::ABORTED &&
             terminal_status != TxStatus::COMMITTED)) {
            return false;
        }

        NodeT* replacement = terminal_status == TxStatus::COMMITTED
            ? record->new_node
            : record->old_node;
        NodeT* detached = terminal_status == TxStatus::COMMITTED
            ? record->old_node
            : record->new_node;
        if (!replacement || !detached) return false;

        HeadWord expected = packRecord_(record);
        if (!head_.compare_exchange_strong(expected,
                                           packNode_(replacement),
                                           std::memory_order_acq_rel,
                                           std::memory_order_acquire)) {
            // Another helper completed this generation, or a later writer
            // installed a new generation.  That thread owns retirement.
            return false;
        }

        // Only the successful Record -> Node CAS retires the detached pair.
        retireOrLeak(detached);
        retireOrLeak(record);
        return true;
    }

    void destroyHead_() noexcept {
        HeadWord raw = head_.load(std::memory_order_acquire);
        if (raw == 0) return;

        if (RecordT* record = unpackRecord_(raw)) {
#if STM_WW_VERIFY_LOGIC_MODE
            delete record->old_node;
            delete record->new_node;
            delete record;
#else
            // While a Record is the root, both nodes are reachable through
            // it.  Destruction is valid only after external users stop.
            retireOrLeak(record->old_node);
            retireOrLeak(record->new_node);
            retireOrLeak(record);
#endif
            return;
        }

        retireOrLeak(unpackNode_(raw));
    }

    RecordT* debugLoadRecord_() const noexcept {
        return unpackRecord_(head_.load(std::memory_order_acquire));
    }

public:
    template<typename... Args>
    explicit TMVar(Args&&... args) : head_(0) {
        NodeT* init_node = new NodeT(0, std::forward<Args>(args)...);
        head_.store(packNode_(init_node), std::memory_order_release);
    }

    ~TMVar() {
#if STM_WW_VERIFY_LOGIC_MODE
        std::lock_guard<std::mutex> lk(verify_mu_);
#endif
        destroyHead_();
    }

    // 禁止拷贝和移动
    TMVar(const TMVar&) = delete;
    TMVar& operator=(const TMVar&) = delete;
    TMVar(TMVar&&) = delete;
    TMVar& operator=(TMVar&&) = delete;

#if STM_WW_TEST_HOOKS
    static void setReadSnapshotHook(ReadSnapshotHook hook) noexcept {
        read_snapshot_hook_.store(hook, std::memory_order_release);
    }

    static void clearReadSnapshotHook() noexcept {
        setReadSnapshotHook(nullptr);
    }

    HeadWord debugLoadHeadForTest() const noexcept {
        return head_.load(std::memory_order_acquire);
    }

    RecordT* debugLoadRecordForTest() const noexcept {
        return unpackRecord_(head_.load(std::memory_order_acquire));
    }

    NodeT* debugLoadNodeForTest() const noexcept {
        return unpackNode_(head_.load(std::memory_order_acquire));
    }

    bool debugHelpCurrentRecordForTest() {
#if STM_WW_VERIFY_LOGIC_MODE
        std::lock_guard<std::mutex> lk(verify_mu_);
#endif
        HeadWord raw = head_.load(std::memory_order_acquire);
        RecordT* record = unpackRecord_(raw);
        if (!record) return false;
        TxStatus status = record->owner->status.load(std::memory_order_acquire);
        return helpResolveRecord_(record, status);
    }
#endif

    ReadSnapshot<T> readSnapshot(const TxDescriptor* tx) {
#if STM_WW_VERIFY_LOGIC_MODE
        std::lock_guard<std::mutex> lk(verify_mu_);
#endif
        for (;;) {
            HeadWord h1 = head_.load(std::memory_order_acquire);
            NodeT* selected = selectVisibleNode_(h1, tx);
            if (selected == nullptr) {
                std::fprintf(stderr, "[FATAL] Var:%p | invalid head state\n",
                             static_cast<void*>(this));
                std::abort();
            }

            // Copy both fields from exactly the same VersionNode.
            T value = selected->payload;
            uint64_t version = selected->loadWriteTs();

#if STM_WW_TEST_HOOKS
            if (auto hook = read_snapshot_hook_.load(std::memory_order_acquire)) {
                hook(*this);
            }
#endif

            HeadWord h2 = head_.load(std::memory_order_acquire);
            if (h1 == h2) {
                return ReadSnapshot<T>{std::move(value), version, selected};
            }
            std::this_thread::yield();
        }
    }

    // Compatibility API.  New transaction code uses readSnapshot() so the
    // value and version are obtained from one generation.
    T readProxy(TxDescriptor* tx) {
        return readSnapshot(tx).value;
    }

    void* tryWriteAndGetRecord(TxDescriptor* tx,
                               const void* val_ptr,
                               TxDescriptor*& out_conflict) {
        out_conflict = nullptr;
        if (!tx || !val_ptr) return nullptr;

#if STM_WW_VERIFY_LOGIC_MODE
        std::lock_guard<std::mutex> lk(verify_mu_);
#endif

        constexpr int kTryWriteRetryLimit = 20000;
        int retry_count = 0;

        while (++retry_count <= kTryWriteRetryLimit) {
            if (tx->status.load(std::memory_order_acquire) != TxStatus::ACTIVE) {
                return nullptr;
            }

            HeadWord observed = head_.load(std::memory_order_acquire);
            if (RecordT* current = unpackRecord_(observed)) {
                if (current->owner == tx) {
                    if (tx->status.load(std::memory_order_acquire) != TxStatus::ACTIVE) {
                        return nullptr;
                    }

                    // The Locator relationship is immutable.  Only its
                    // owner's ACTIVE draft payload may be updated.
                    static_assert(std::is_assignable_v<T&, const T&>,
                                  "re-entrant WwSTM writes require assignable T");
                    current->new_node->payload = *static_cast<const T*>(val_ptr);

                    if (tx->status.load(std::memory_order_acquire) != TxStatus::ACTIVE) {
                        return nullptr;
                    }
                    return current;
                }

                TxStatus status = current->owner->status.load(std::memory_order_acquire);
                if (status == TxStatus::ACTIVE) {
                    out_conflict = current->owner;
                    return nullptr;
                }

                // Terminal records are first flattened to a Node.  The next
                // loop then attempts Node -> Record; Record -> Record is gone.
                helpResolveRecord_(current, status);
                std::this_thread::yield();
                continue;
            }

            NodeT* old_node = unpackNode_(observed);
            if (!old_node) continue;

            NodeT* new_node = new NodeT(tx->start_ts,
                                        *static_cast<const T*>(val_ptr));
            RecordT* record = new RecordT(tx, old_node, new_node);
            HeadWord expected = observed;
            if (head_.compare_exchange_strong(expected,
                                               packRecord_(record),
                                               std::memory_order_acq_rel,
                                               std::memory_order_acquire)) {
                // The successful CAS is the publication point.  No field of
                // record is modified after this point.
                return record;
            }

            // This candidate was never visible through head_ and can be
            // destroyed directly.
            delete new_node;
            delete record;
            std::this_thread::yield();
        }

        return nullptr;
    }

    bool ownsRecord(void* saved_record_ptr,
                    const TxDescriptor* owner) const override {
        auto* record = static_cast<RecordT*>(saved_record_ptr);
        if (!record || !owner) return false;
#if STM_WW_VERIFY_LOGIC_MODE
        std::lock_guard<std::mutex> lk(verify_mu_);
#endif
        if (head_.load(std::memory_order_acquire) != packRecord_(record)) {
            return false;
        }
        return record->owner == owner;
    }

    bool validateReadSnapshot(const void* node_ptr,
                              uint64_t version,
                              const TxDescriptor* tx) const override {
        if (!node_ptr) return false;
        ReadSnapshot<T> snapshot = const_cast<TMVar*>(this)->readSnapshot(tx);
        return snapshot.node == node_ptr && snapshot.version == version;
    }

    bool validateReadBeforeWrite(void* saved_record_ptr,
                                 const void* node_ptr,
                                 uint64_t version) const override {
        auto* record = static_cast<RecordT*>(saved_record_ptr);
        if (!record || !node_ptr) return false;
#if STM_WW_VERIFY_LOGIC_MODE
        std::lock_guard<std::mutex> lk(verify_mu_);
#endif
        if (head_.load(std::memory_order_acquire) != packRecord_(record)) {
            return false;
        }
        return record->old_node == node_ptr
            && record->old_node->loadWriteTs() == version;
    }

    bool commitReleaseRecord(void* saved_record_ptr,
                             uint64_t commit_ts) override {
#if STM_WW_VERIFY_LOGIC_MODE
        std::lock_guard<std::mutex> lk(verify_mu_);
#endif
        auto* record = static_cast<RecordT*>(saved_record_ptr);
        if (!record) return false;

        // A terminal record may already have been flattened by a helper
        // before the owner reaches this compatibility cleanup path.  The
        // write set still contains the raw token until cleanupResources(),
        // so do not dereference it unless it is still the current head.
        HeadWord expected = packRecord_(record);
        if (head_.load(std::memory_order_acquire) != expected) {
            return true;
        }

        // Commit A compatibility: the timestamp is still supplied by the
        // legacy post-COMMITTED cleanup caller.  Commit B moves this store to
        // prepare, before the status CAS.
        record->new_node->storeWriteTs(commit_ts);

        if (head_.compare_exchange_strong(expected,
                                          packNode_(record->new_node),
                                          std::memory_order_acq_rel,
                                          std::memory_order_acquire)) {
            retireOrLeak(record->old_node);
            retireOrLeak(record);
        }

        // Once status is COMMITTED, losing this CAS means a helper or a later
        // generation completed the physical flatten.  It is not a failure.
        return true;
    }

    // 兼容旧测试调用方式：直接提交当前锁记录。
    bool commitReleaseRecord(uint64_t commit_ts) {
        RecordT* current = debugLoadRecord_();
        return commitReleaseRecord(static_cast<void*>(current), commit_ts);
    }

    void abortRestoreData(void* saved_record_ptr) override {
        auto* record = static_cast<RecordT*>(saved_record_ptr);
        if (!record) return;
#if STM_WW_VERIFY_LOGIC_MODE
        std::lock_guard<std::mutex> lk(verify_mu_);
#endif

        HeadWord expected = packRecord_(record);
        // A helper may have already completed Record -> Node and retired the
        // record.  The owner still holds the raw write-set token until the
        // transaction cleanup completes, so an early head check is required
        // before reading old_node/new_node from that possibly stale token.
        if (head_.load(std::memory_order_acquire) != expected) {
            return;
        }

        NodeT* old_node = record->old_node;
        NodeT* new_node = record->new_node;
        if (head_.compare_exchange_strong(expected,
                                          packNode_(old_node),
                                          std::memory_order_acq_rel,
                                          std::memory_order_acquire)) {
            retireOrLeak(new_node);
            retireOrLeak(record);
        }
    }

    uint64_t getDataVersion() const override {
#if STM_WW_VERIFY_LOGIC_MODE
        std::lock_guard<std::mutex> lk(verify_mu_);
#endif
        NodeT* node = loadVisibleNode_(nullptr);
        if (node == nullptr) {
            std::fprintf(stderr, "[FATAL] Var:%p | head resolved to NULL!\n",
                         static_cast<const void*>(this));
            std::abort();
        }

        if ((reinterpret_cast<uintptr_t>(node) & 0x1) != 0) {
            std::fprintf(stderr,
                         "[FATAL] Var:%p | Corrupt node pointer in getDataVersion: %p\n",
                         static_cast<const void*>(this),
                         static_cast<void*>(node));
        }

        return node->loadWriteTs();
    }
};

} // namespace Ww
} // namespace STM
