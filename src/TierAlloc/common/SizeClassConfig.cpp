#include "common/SizeClassConfig.hpp"
#include <mutex>
#include <cassert>


// 静态成员变量定义
size_t SizeClassConfig::class_to_size_[kClassCount];

void SizeClassConfig::Init() {
    static std::once_flag flag;
    std::call_once(flag, []() {
        size_t index = 0;
        
        // 辅助 lambda：安全填充数组
        auto push_range = [&](size_t start, size_t end, size_t step) {
            for (size_t s = start; s <= end; s += step) {
                if (index < kClassCount) {
                    class_to_size_[index++] = s;
                }
            }
        };

        push_range(8, 128, 8);
        push_range(144, 256, 16);
        push_range(288, 512, 32);

        push_range(576, 1024, 64);
        push_range(1152, 2048, 128);
        push_range(2304, 4096, 256);


        push_range(4608, 8192, 512);
        push_range(9216, 16384, 1024);
        push_range(18432, 32768, 2048);

        push_range(36864, 65536, 4096);
        push_range(73728, 131072, 8192);
        push_range(147456, 262144, 16384);

        assert(index == kClassCount && "SizeClassConfig: Class count mismatch!");
    });
}

size_t SizeClassConfig::SizeToClass(size_t nbytes) noexcept {
    // 0. 异常/边界处理
    if (nbytes <= kMinAlloc) return 0; // 0-8 bytes -> index 0
    if (nbytes > kMaxAlloc) return kClassCount; // Out of bounds

    // 1. 极速路径 (Tiny Objects <= 128B)
    // 对应 Init 中的第一阶段，直接位运算计算下标
    // (nbytes - 1) / 8 等价于 (nbytes - 1) >> 3
    if (nbytes <= 128) {
        return (nbytes - 1) >> 3;
    }

    // 2. 常规路径 (Binary Search)
    // 由于数组有序，且我们已知前 16 个 slot 是线性的，可以从 index 16 开始搜索
    // 数组范围: class_to_size_
    
    int left = 16;
    int right = kClassCount - 1;

    // 手写二分查找 (Lower Bound)，寻找第一个 >= nbytes 的位置
    while (left < right) {
        int mid = (left + right) / 2;
        if (class_to_size_[mid] < nbytes) {
            left = mid + 1;
        } else {
            right = mid;
        }
    }

    return static_cast<size_t>(left);
}

size_t SizeClassConfig::ClassToSize(size_t class_idx) noexcept {
    // Debug 模式下检查越界，Release 模式下为了性能不检查（由调用者保证）
    assert(class_idx < kClassCount);
    return class_to_size_[class_idx];
}

size_t SizeClassConfig::Normalize(size_t nbytes) noexcept {
    // 大于 256KB，按页对齐
    if (nbytes > kMaxAlloc) {
        return RoundUp_(nbytes, kPageSize);
    }
    
    // 小内存查表获取实际 Block 大小
    size_t idx = SizeToClass(nbytes);
    return ClassToSize(idx);
}
