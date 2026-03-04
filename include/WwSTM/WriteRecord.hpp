#pragma once

#include <cstdint>
#include <utility>
#include <variant>
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

    WriteRecord(TxDescriptor* tx, VersionNode<T>* old_v, VersionNode<T>* new_v)
        : owner(tx)
        , old_node(old_v)
        , new_node(new_v)
    {}

    WriteRecord(const WriteRecord&) = delete;
    WriteRecord& operator=(const WriteRecord&) = delete;

    static void* operator new(size_t size) {
        return ::operator new(size);
    }

    static void operator delete(void* p) {
        ::operator delete(p);
    }
};


}
}
}
