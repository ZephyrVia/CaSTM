#pragma once

#include <cstdint>
#include <type_traits>
#include <cassert>

namespace STM {
namespace Ww {

struct TaggedPtrHelper {
    static constexpr uintptr_t kRecordTag = 1;
    static constexpr uintptr_t kPtrMask = ~kRecordTag;

    // 检查当前 raw 指针是否指向 WriteRecord (是否被标记)
    static bool isRecord(uintptr_t raw) {
        return (raw & kRecordTag) != 0;
    }

    // 检查当前 raw 指针是否指向 VersionNode (是否未标记)
    static bool isNode(uintptr_t raw) {
        return (raw & kRecordTag) == 0;
    }

    // 打包node：保持原样
    template<typename T> 
    static uintptr_t packNode(T* node_ptr) {
        assert(isNode((reinterpret_cast<uintptr_t>(node_ptr))));
        return reinterpret_cast<uintptr_t>(node_ptr);
    }

    // 打包record：最后一位打上标记
    template<typename T>
    static uintptr_t packRecord(T* record_ptr) {
        assert(isNode((reinterpret_cast<uintptr_t>(record_ptr))));
        return reinterpret_cast<uintptr_t>(record_ptr) | kRecordTag;
    }

    // 从uintptr_t解包出node指针
    template<typename NodeT>
    static NodeT* unpackNode(uintptr_t raw) {
        assert(isNode(raw));
        return reinterpret_cast<NodeT*>(raw);
    }

    // 从uintptr_t解包出record指针
    template<typename RecordT>
    static RecordT* unpackRecord(uintptr_t raw) {
        assert(isRecord(raw));
        return reinterpret_cast<RecordT*>(raw & kPtrMask);
    }
};

}
}