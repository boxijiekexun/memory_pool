#include "../include/CentralCache.h"
#include "../include/PageCache.h"
#include <cassert>
#include <cstdint>
#include <thread>

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
        void* head = centralFreeList_[index].load(std::memory_order_relaxed);
        if (head) {
            // 先确定批次边界并验证追踪记录，再统一更新计数和中央链表。
            std::array<SpanTracker*, BATCH_SIZE> batchTrackers{};
            std::array<size_t, BATCH_SIZE> uniqueCounts{};
            size_t uniqueTrackerCount = 0;
            size_t batchCount = 0;
            void* tail = nullptr;
            void* current = head;
            while (current && batchCount < BATCH_SIZE) {
                SpanTracker* tracker = getSpanTracker(index, current);
                if (!tracker) {
                    assert(false && "central free-list block has invalid Span tracking");
                    locks_[index].clear(std::memory_order_release);
                    return nullptr;
                }
                size_t trackerPosition = 0;
                while (trackerPosition < uniqueTrackerCount &&
                       batchTrackers[trackerPosition] != tracker)
                    ++trackerPosition;
                if (trackerPosition == uniqueTrackerCount) {
                    batchTrackers[uniqueTrackerCount] = tracker;
                    uniqueCounts[uniqueTrackerCount] = 0;
                    ++uniqueTrackerCount;
                }
                ++uniqueCounts[trackerPosition];
                tail = current;
                current = *reinterpret_cast<void**>(current);
                ++batchCount;
            }

            for (size_t i = 0; i < uniqueTrackerCount; ++i) {
                if (batchTrackers[i]->freeCount < uniqueCounts[i]) {
                    assert(false && "central free count does not cover fetched batch");
                    locks_[index].clear(std::memory_order_release);
                    return nullptr;
                }
            }
            for (size_t i = 0; i < uniqueTrackerCount; ++i) {
                batchTrackers[i]->freeCount -= uniqueCounts[i];
            }
            centralFreeList_[index].store(current, std::memory_order_release);
            *reinterpret_cast<void**>(tail) = nullptr;
            result = head;
        } else {
            const size_t blockSize = (index + 1) * ALIGNMENT;
            const size_t numPages = blockSize <= SPAN_PAGES * PageCache::PAGE_SIZE
                ? SPAN_PAGES
                : (blockSize + PageCache::PAGE_SIZE - 1) / PageCache::PAGE_SIZE;
            result = PageCache::getInstance().allocateSpan(numPages);
            if (result) {
                const size_t blockCount = numPages * PageCache::PAGE_SIZE / blockSize;
                const size_t batchCount = std::min(blockCount, BATCH_SIZE);
                char* start = static_cast<char*>(result);
                void* centralHead = batchCount < blockCount
                    ? start + batchCount * blockSize : nullptr;
                void* batchTail = nullptr;

                for (size_t i = 0; i < blockCount; ++i) {
                    void* block = start + i * blockSize;
                    void* next = (i + 1 < blockCount) ? start + (i + 1) * blockSize : nullptr;
                    *reinterpret_cast<void**>(block) = next;
                    if (i + 1 == batchCount) batchTail = block;
                }

                if (batchTail) *reinterpret_cast<void**>(batchTail) = nullptr;

                try {
                    spanTrackers_[index].push_back(
                        SpanTracker{start, numPages, blockCount, blockCount - batchCount});
                } catch (...) {
                    PageCache::getInstance().deallocateSpan(result, numPages);
                    result = nullptr;
                }

                if (result) {
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
        // 先确定链表长度并验证每个节点都有追踪记录。
        void* end = start;
        size_t returnedBlocks = 0;
        while (returnedBlocks < expectedBlocks) {
            if (!getSpanTracker(index, end)) {
                assert(false && "returned block has no Span tracker");
                locks_[index].clear(std::memory_order_release);
                return;
            }
            ++returnedBlocks;

            void* next = *reinterpret_cast<void**>(end);
            if (!next) {
                break;
            }
            if (returnedBlocks == expectedBlocks) break;
            end = next;
        }

        // 对每个 Span 统计本批次归还的块数。不使用动态临时容器，
        // 这样 TLS 析构归还路径不会因临时分配失败而丢失本地链表。
        for (const SpanTracker& tracker : spanTrackers_[index]) {
            const uintptr_t begin = reinterpret_cast<uintptr_t>(tracker.spanAddr);
            const uintptr_t endAddress = begin + tracker.numPages * PageCache::PAGE_SIZE;
            size_t count = 0;
            void* current = start;
            for (size_t i = 0; i < returnedBlocks; ++i) {
                const uintptr_t address = reinterpret_cast<uintptr_t>(current);
                if (address >= begin && address < endAddress) ++count;
                current = *reinterpret_cast<void**>(current);
            }
            if (tracker.freeCount + count > tracker.blockCount) {
                assert(false && "returned batch contains a duplicate free block");
                locks_[index].clear(std::memory_order_release);
                return;
            }
        }

        void* oldHead = centralFreeList_[index].load(std::memory_order_relaxed);
        *reinterpret_cast<void**>(end) = oldHead;
        centralFreeList_[index].store(start, std::memory_order_release);

        for (SpanTracker& tracker : spanTrackers_[index]) {
            const uintptr_t begin = reinterpret_cast<uintptr_t>(tracker.spanAddr);
            const uintptr_t endAddress = begin + tracker.numPages * PageCache::PAGE_SIZE;
            size_t count = 0;
            void* current = start;
            for (size_t i = 0; i < returnedBlocks; ++i) {
                const uintptr_t address = reinterpret_cast<uintptr_t>(current);
                if (address >= begin && address < endAddress) ++count;
                current = *reinterpret_cast<void**>(current);
            }
            tracker.freeCount += count;
        }

        // 只在整批计数更新完后回收 Span，避免释放后继续遍历该批次中的节点。
        auto& trackers = spanTrackers_[index];
        for (size_t trackerIndex = 0; trackerIndex < trackers.size();) {
            if (trackers[trackerIndex].freeCount == trackers[trackerIndex].blockCount)
                releaseSpan(index, trackerIndex);
            else
                ++trackerIndex;
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

void CentralCache::releaseSpan(size_t index, size_t trackerIndex)
{
    const SpanTracker& tracker = spanTrackers_[index][trackerIndex];
    assert(tracker.freeCount == tracker.blockCount);
    if (tracker.freeCount != tracker.blockCount) return;

    const uintptr_t begin = reinterpret_cast<uintptr_t>(tracker.spanAddr);
    const uintptr_t end = begin + tracker.numPages * PageCache::PAGE_SIZE;
    void* spanAddr = tracker.spanAddr;
    const size_t numPages = tracker.numPages;

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

    spanTrackers_[index].erase(spanTrackers_[index].begin() + trackerIndex);
    PageCache::getInstance().deallocateSpan(spanAddr, numPages);
}

} // namespace memoryPool
