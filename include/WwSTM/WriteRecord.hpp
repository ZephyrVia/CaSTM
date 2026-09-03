#pragma once

#include <cstdint>
#include <utility>
#include <variant>
#include <atomic>
#include "WwSTM/Config.hpp"
#include "TxDescriptor.hpp" 
#include "WwSTM/VersionNode.hpp"

namespace STM {
namespace Ww {
namespace detail {

template<typename T>
struct WriteRecord {
    TxDescriptor* owner;    
    VersionNode<T>* old_node;
    VersionNode<T>* new_node;
    std::atomic<bool> prepared;

    WriteRecord(TxDescriptor* tx, VersionNode<T>* old_v, VersionNode<T>* new_v)
        : owner(tx)
        , old_node(old_v)
        , new_node(new_v)
        , prepared(false)
    {}

    WriteRecord(const WriteRecord&) = delete;
    WriteRecord& operator=(const WriteRecord&) = delete;

    static void* operator new(size_t size) {
        return ::operator new(size);
    }

    static void operator delete(void* p) {
        ::operator delete(p);
    }

#if STM_WW_TEST_HOOKS
    // Used only by the deterministic published-record lifetime regression.
    // A record destructor must run once, after its EBR grace period.
    inline static std::atomic<uint64_t> debug_destructor_count{0};

    ~WriteRecord() {
        debug_destructor_count.fetch_add(1, std::memory_order_relaxed);
    }
#endif
};


}
}
}
