#pragma once
#include <algorithm>
#include <cstddef>
#include <atomic>
#include <array>
#include <cstdint>

namespace Kama_memoryPool 
{
// 对齐数和大小定义
constexpr size_t ALIGNMENT = alignof(std::max_align_t);
constexpr size_t MAX_BYTES = 256 * 1024; // 256KB
constexpr size_t FREE_LIST_SIZE = MAX_BYTES / ALIGNMENT; // ALIGNMENT等于指针void*的大小


// 内存块头部信息
struct BlockHeader
{
    size_t size; // 内存块大小
    bool   inUse; // 使用标志
    BlockHeader* next; // 指向下一个内存块
};

// 大小类管理
class SizeClass 
{
public:
    // 将字节数向上取整到最近的对齐大小
    static size_t roundUp(size_t bytes)
    {
        return (bytes + ALIGNMENT - 1) & ~(ALIGNMENT - 1);
    }

    // 根据字节数获取对应的索引
    // 索引 从0 开始
    // 例如申请 一个25byte 的空间，向上取需要大小为32byte，索引为3
    static size_t getIndex(size_t bytes)
    {   
        // 确保bytes至少为ALIGNMENT
        bytes = std::max(bytes, ALIGNMENT);
        // 向上取整后-1
        return (bytes + ALIGNMENT - 1) / ALIGNMENT - 1;
    }
};

} // namespace memoryPool
