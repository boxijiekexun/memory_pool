#include "../include/CentralCache.h"
#include "../include/MemoryPool.h"
#include "../include/PageCache.h"

#include <cstdint>
#include <iostream>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

using namespace Kama_memoryPool;

namespace {

int testSpanReclaimPreservesOtherSpans()
{
    auto& central = CentralCache::getInstance();
    const size_t blockSize = ALIGNMENT;
    const size_t index = SizeClass::getIndex(blockSize);
    const size_t blocksPerSpan = 8 * PageCache::PAGE_SIZE / blockSize;
    std::vector<void*> firstSpanBlocks;
    firstSpanBlocks.reserve(blocksPerSpan);

    // Exhaust one Span in batches so the next fetch creates a second Span.
    while (firstSpanBlocks.size() < blocksPerSpan) {
        void* batch = central.fetchRange(index);
        if (!batch) return 1;
        for (void* current = batch; current; current = *reinterpret_cast<void**>(current))
            firstSpanBlocks.push_back(current);
    }
    if (firstSpanBlocks.size() != blocksPerSpan) return 1;

    void* secondSpanBatch = central.fetchRange(index);
    if (!secondSpanBatch) return 1;
    const void* expectedSecondSpanRemainder =
        static_cast<char*>(secondSpanBatch) + CentralCache::BATCH_SIZE * blockSize;

    for (size_t i = 0; i < firstSpanBlocks.size(); ++i) {
        *reinterpret_cast<void**>(firstSpanBlocks[i]) =
            i + 1 < firstSpanBlocks.size() ? firstSpanBlocks[i + 1] : nullptr;
    }
    central.returnRange(firstSpanBlocks.front(), firstSpanBlocks.size() * blockSize, index);

    // 回收第一个 Span 后，第二个 Span 的中央链表节点仍应可取到。
    void* secondSpanNextBatch = central.fetchRange(index);
    if (secondSpanNextBatch != expectedSecondSpanRemainder) {
        std::cerr << "reclaiming one span lost another span's central free block\n";
        return 1;
    }

    // 归还第二个 Span 已分配给测试的两个批次，覆盖跨 Span 批量传输。
    void* secondSpanTail = secondSpanBatch;
    while (*reinterpret_cast<void**>(secondSpanTail))
        secondSpanTail = *reinterpret_cast<void**>(secondSpanTail);
    *reinterpret_cast<void**>(secondSpanTail) = secondSpanNextBatch;
    size_t secondBatchCount = 0;
    for (void* current = secondSpanNextBatch; current;
         current = *reinterpret_cast<void**>(current))
        ++secondBatchCount;
    central.returnRange(secondSpanBatch,
                        (CentralCache::BATCH_SIZE + secondBatchCount) * blockSize, index);
    return 0;
}

int testTrackerCapacityAndRetirement()
{
    auto& central = CentralCache::getInstance();
    const size_t blockSize = 16 * 1024; // 每个 8 页 Span 含两个块
    const size_t index = SizeClass::getIndex(blockSize);
    const size_t spanCount = 1030;
    std::vector<void*> blocks;
    blocks.reserve(spanCount * 2);

    for (size_t i = 0; i < spanCount; ++i) {
        void* block = central.fetchRange(index);
        if (!block) {
            std::cerr << "failed while creating more than 1024 tracked spans\n";
            return 1;
        }
        size_t batchCount = 0;
        for (void* current = block; current; current = *reinterpret_cast<void**>(current)) {
            blocks.push_back(current);
            ++batchCount;
        }
        if (batchCount != 2) return 1;
    }

    for (size_t i = 0; i < blocks.size(); ++i) {
        *reinterpret_cast<void**>(blocks[i]) =
            (i + 1 < blocks.size()) ? blocks[i + 1] : nullptr;
    }
    central.returnRange(blocks.front(), blocks.size() * blockSize, index);

    // 所有追踪记录均应退休；同一大小类仍可重新申请和归还。
    void* block = central.fetchRange(index);
    if (!block) return 1;
    size_t batchCount = 0;
    for (void* current = block; current; current = *reinterpret_cast<void**>(current))
        ++batchCount;
    central.returnRange(block, batchCount * blockSize, index);
    return 0;
}

int testThreadCacheAlignmentAndDoubleFree()
{
    void* p = MemoryPool::allocate(37);
    if (!p || reinterpret_cast<uintptr_t>(p) % alignof(std::max_align_t) != 0) {
        std::cerr << "small allocation does not satisfy max_align_t alignment\n";
        return 1;
    }

    MemoryPool::deallocate(p, 37);
    MemoryPool::deallocate(p, 37); // 当前 ThreadCache 中的重复释放应被忽略

    void* a = MemoryPool::allocate(37);
    void* b = MemoryPool::allocate(37);
    if (!a || !b || a == b) {
        std::cerr << "double free allowed the same block to be allocated twice\n";
        return 1;
    }

    MemoryPool::deallocate(nullptr, 37);
    MemoryPool::deallocate(a, 37);
    MemoryPool::deallocate(b, 37);
    return 0;
}

bool runIsolated(int (*test)())
{
    const pid_t child = fork();
    if (child < 0) return false;
    if (child == 0) _exit(test());

    int status = 0;
    return waitpid(child, &status, 0) == child && WIFEXITED(status) &&
           WEXITSTATUS(status) == 0;
}

} // namespace

int main()
{
    if (!runIsolated(testSpanReclaimPreservesOtherSpans) ||
        !runIsolated(testTrackerCapacityAndRetirement) ||
        !runIsolated(testThreadCacheAlignmentAndDoubleFree)) {
        std::cerr << "Allocator safety regression test failed\n";
        return 1;
    }
    std::cout << "Allocator safety regression tests passed.\n";
    return 0;
}
