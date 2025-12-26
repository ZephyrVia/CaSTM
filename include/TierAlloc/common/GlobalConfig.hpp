
#pragma once

#include <cstddef>

// =========================================================
// 全局配置参数 (Global Configuration)
// =========================================================

// Chunk 大小与对齐要求：2MB (Huge Page 兼容大小)
// 这是整个分配器的核心粒度
constexpr size_t kChunkSize = 2 * 1024 * 1024;


// 对应的对齐要求 (通常与 Chunk 大小一致，便于位运算寻址)
constexpr size_t kChunkAlignment = kChunkSize;

// 掩码：用于通过任意指针快速计算 Chunk 头部位置
// 原理：ptr & kChunkMask
constexpr size_t kChunkMask = ~(kChunkSize - 1);


// CentralHeap 最大缓存 Chunk 数量
constexpr size_t kMaxCentralCacheSize = 64;

// 缓存行大小
inline constexpr size_t kCacheLineSize = 64;


constexpr size_t kMaxPoolRescueChecks = 4;

constexpr size_t kMaxThreadCacheSize = 8;