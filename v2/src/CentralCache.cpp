#include "../include/CentralCache.h"
#include "../include/PageCache.h"
#include <cassert>
#include <cstdint>
#include <thread>
#include <unordered_map>

namespace Kama_memoryPool
{

// 每个大小类通常从 PageCache 获取 8 页；更大的块按需获取整页。
static const size_t SPAN_PAGES = 8;

CentralCache::CentralCache()
{
    for (auto& ptr : centralFreeList_) ptr.store(nullptr, std::memory_order_relaxed);
    for (auto& lock : locks_) lock.clear();
}

void* CentralCache::fetchRange(size_t index)
{
    if (index >= FREE_LIST_SIZE) return nullptr;

    while (locks_[index].test_and_set(std::memory_order_acquire))
        std::this_thread::yield();

    void* result = nullptr;
    try {
        result = centralFreeList_[index].load(std::memory_order_relaxed);
        if (result) {
            void* next = *reinterpret_cast<void**>(result);
            centralFreeList_[index].store(next, std::memory_order_release);
            *reinterpret_cast<void**>(result) = nullptr;

            SpanTracker* tracker = getSpanTracker(index, result);
            if (tracker) {
                assert(tracker->freeCount > 0);
                --tracker->freeCount;
            }
        } else {
            const size_t blockSize = (index + 1) * ALIGNMENT;
            const size_t numPages = blockSize <= SPAN_PAGES * PageCache::PAGE_SIZE
                ? SPAN_PAGES
                : (blockSize + PageCache::PAGE_SIZE - 1) / PageCache::PAGE_SIZE;
            result = PageCache::getInstance().allocateSpan(numPages);
            if (result) {
                const size_t blockCount = numPages * PageCache::PAGE_SIZE / blockSize;
                char* start = static_cast<char*>(result);
                void* centralHead = nullptr;

                for (size_t i = 1; i < blockCount; ++i) {
                    void* block = start + i * blockSize;
                    void* next = (i + 1 < blockCount) ? start + (i + 1) * blockSize : nullptr;
                    *reinterpret_cast<void**>(block) = next;
                    if (i == 1) centralHead = block;
                }

                try {
                    spanTrackers_[index].push_back(
                        SpanTracker{start, numPages, blockCount, blockCount - 1});
                } catch (...) {
                    PageCache::getInstance().deallocateSpan(result, numPages);
                    result = nullptr;
                }

                if (result) {
                    *reinterpret_cast<void**>(result) = nullptr;
                    centralFreeList_[index].store(centralHead, std::memory_order_release);
                }
            }
        }
    } catch (...) {
        locks_[index].clear(std::memory_order_release);
        throw;
    }

    locks_[index].clear(std::memory_order_release);
    return result;
}

void CentralCache::returnRange(void* start, size_t size, size_t index)
{
    if (!start || index >= FREE_LIST_SIZE) return;

    const size_t blockSize = (index + 1) * ALIGNMENT;
    const size_t expectedBlocks = size / blockSize;
    if (expectedBlocks == 0) return;

    while (locks_[index].test_and_set(std::memory_order_acquire))
        std::this_thread::yield();

    try {
        // 归还链表可能由多个 Span 的块组成，按 Span 起始地址统计本次归还量。
        std::unordered_map<void*, size_t> returnedBySpan;
        void* end = start;
        size_t returnedBlocks = 0;
        while (returnedBlocks < expectedBlocks) {
            SpanTracker* tracker = getSpanTracker(index, end);
            if (tracker) ++returnedBySpan[tracker->spanAddr];
            ++returnedBlocks;

            void* next = *reinterpret_cast<void**>(end);
            if (!next) {
                break;
            }
            if (returnedBlocks == expectedBlocks) break;
            end = next;
        }

        void* oldHead = centralFreeList_[index].load(std::memory_order_relaxed);
        *reinterpret_cast<void**>(end) = oldHead;
        centralFreeList_[index].store(start, std::memory_order_release);

        for (const auto& item : returnedBySpan) {
            SpanTracker* tracker = getSpanTracker(index, item.first);
            if (tracker) updateSpanFreeCount(index, tracker, item.second);
        }
    } catch (...) {
        locks_[index].clear(std::memory_order_release);
        throw;
    }

    locks_[index].clear(std::memory_order_release);
}

SpanTracker* CentralCache::getSpanTracker(size_t index, void* blockAddr)
{
    const uintptr_t address = reinterpret_cast<uintptr_t>(blockAddr);
    for (SpanTracker& tracker : spanTrackers_[index]) {
        const uintptr_t begin = reinterpret_cast<uintptr_t>(tracker.spanAddr);
        const uintptr_t end = begin + tracker.numPages * PageCache::PAGE_SIZE;
        if (address >= begin && address < end) return &tracker;
    }
    return nullptr;
}

void CentralCache::updateSpanFreeCount(size_t index, SpanTracker* tracker,
                                       size_t returnedBlocks)
{
    tracker->freeCount += returnedBlocks;
    assert(tracker->freeCount <= tracker->blockCount);
    if (tracker->freeCount != tracker->blockCount) return;

    const uintptr_t begin = reinterpret_cast<uintptr_t>(tracker->spanAddr);
    const uintptr_t end = begin + tracker->numPages * PageCache::PAGE_SIZE;
    const void* spanAddr = tracker->spanAddr;
    const size_t numPages = tracker->numPages;

    // 摘除该 Span 的所有节点，同时保留其他 Span 的链表头和节点。
    void* head = centralFreeList_[index].load(std::memory_order_relaxed);
    void* newHead = head;
    void* previous = nullptr;
    void* current = head;
    while (current) {
        void* next = *reinterpret_cast<void**>(current);
        const uintptr_t address = reinterpret_cast<uintptr_t>(current);
        if (address >= begin && address < end) {
            if (previous) *reinterpret_cast<void**>(previous) = next;
            else newHead = next;
        } else {
            previous = current;
        }
        current = next;
    }
    centralFreeList_[index].store(newHead, std::memory_order_release);

    auto& trackers = spanTrackers_[index];
    auto found = std::find_if(trackers.begin(), trackers.end(),
        [spanAddr](const SpanTracker& item) { return item.spanAddr == spanAddr; });
    if (found != trackers.end()) trackers.erase(found);

    PageCache::getInstance().deallocateSpan(const_cast<void*>(spanAddr), numPages);
}

} // namespace memoryPool
