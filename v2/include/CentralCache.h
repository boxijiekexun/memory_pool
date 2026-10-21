#pragma once
#include "Common.h"
#include <array>
#include <atomic>
#include <vector>

// 所有线程共享的缓存
namespace Kama_memoryPool
{

struct SpanTracker {
    void* spanAddr = nullptr;
    size_t numPages = 0;
    size_t blockCount = 0;
    // 只统计当前位于 CentralCache 空闲链表中的块。
    size_t freeCount = 0;
};

class CentralCache
{
public:
    static CentralCache& getInstance()
    {
        static CentralCache instance;
        return instance;
    }

    void* fetchRange(size_t index);
    void returnRange(void* start, size_t size, size_t index);

private:
    // 相互是还所有原子指针为nullptr
    CentralCache();
    // 从页缓存获取内存
    void* fetchFromPageCache(size_t size);

    // 在对应大小类的锁保护下，查找包含块地址的 Span。
    SpanTracker* getSpanTracker(size_t index, void* blockAddr);

    // 更新精确的中央空闲块计数；全空闲时从链表移除并归还 PageCache。
    void updateSpanFreeCount(size_t index, SpanTracker* tracker, size_t returnedBlocks);

private:
    // 中心缓存的自由链表
    std::array<std::atomic<void*>, FREE_LIST_SIZE> centralFreeList_;

    // 用于同步的自旋锁
    std::array<std::atomic_flag, FREE_LIST_SIZE> locks_;
    
    // 一个大小类锁同时保护该类链表及其 Span 元数据。
    std::array<std::vector<SpanTracker>, FREE_LIST_SIZE> spanTrackers_;
};

} // namespace memoryPool
