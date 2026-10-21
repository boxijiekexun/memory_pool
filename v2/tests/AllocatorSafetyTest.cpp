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
    const size_t blockSize = 16 * 1024;
    const size_t index = SizeClass::getIndex(blockSize);

    void* a0 = central.fetchRange(index);
    void* a1 = central.fetchRange(index);
    void* b0 = central.fetchRange(index);
    if (!a0 || !a1 || !b0) return 1;

    void* expectedB1 = static_cast<char*>(b0) + blockSize;
    *reinterpret_cast<void**>(a0) = a1;
    *reinterpret_cast<void**>(a1) = nullptr;
    central.returnRange(a0, 2 * blockSize, index);

    // A Span 的完整回收不能丢掉中央链表头部属于 B Span 的空闲块。
    void* b1 = central.fetchRange(index);
    if (b1 != expectedB1) {
        std::cerr << "reclaiming one span lost another span's central free block\n";
        return 1;
    }

    *reinterpret_cast<void**>(b0) = b1;
    *reinterpret_cast<void**>(b1) = nullptr;
    central.returnRange(b0, 2 * blockSize, index);
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

    for (size_t i = 0; i < spanCount * 2; ++i) {
        void* block = central.fetchRange(index);
        if (!block) {
            std::cerr << "failed while creating more than 1024 tracked spans\n";
            return 1;
        }
        blocks.push_back(block);
    }

    for (size_t i = 0; i < blocks.size(); ++i) {
        *reinterpret_cast<void**>(blocks[i]) =
            (i + 1 < blocks.size()) ? blocks[i + 1] : nullptr;
    }
    central.returnRange(blocks.front(), blocks.size() * blockSize, index);

    // 所有追踪记录均应退休；同一大小类仍可重新申请和归还。
    void* block = central.fetchRange(index);
    if (!block) return 1;
    *reinterpret_cast<void**>(block) = nullptr;
    central.returnRange(block, blockSize, index);
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
