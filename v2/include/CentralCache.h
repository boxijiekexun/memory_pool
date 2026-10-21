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
    static constexpr size_t BATCH_SIZE = 32;

    static CentralCache& getInstance()
    {
        // ThreadCache TLS 析构期间仍可能调用 CentralCache。
        static CentralCache* instance = new CentralCache;
        return *instance;
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

    // 归还一个已完全空闲的 Span，并退休其追踪记录。
    void releaseSpan(size_t index, size_t trackerIndex);

private:
    // 中心缓存的自由链表
    std::array<std::atomic<void*>, FREE_LIST_SIZE> centralFreeList_;

    // 用于同步的自旋锁
    std::array<std::atomic_flag, FREE_LIST_SIZE> locks_;
    
    // 一个大小类锁同时保护该类链表及其 Span 元数据。
    std::array<std::vector<SpanTracker>, FREE_LIST_SIZE> spanTrackers_;
};

} // namespace memoryPool
