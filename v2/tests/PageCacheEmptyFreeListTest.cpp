/*
 * 回归测试 PageCache 的空闲链表桶处理：
 * 先申请 8 页并归还，再分割为 6 页和 2 页；保持右侧 2 页 Span 在用，
 * 归还左侧 6 页后再次申请 2 页。测试要求子进程正常退出，且新分配
 * 有效并且不与仍在使用的右侧 Span 重叠。
 */
#include "../include/PageCache.h"

#include <cstdint>
#include <iostream>
#include <sys/wait.h>
#include <unistd.h>

using namespace Kama_memoryPool;

namespace {

bool rangesOverlap(const void* first, size_t firstSize,
                   const void* second, size_t secondSize)
{
    const auto firstBegin = reinterpret_cast<uintptr_t>(first);
    const auto secondBegin = reinterpret_cast<uintptr_t>(second);
    return firstBegin < secondBegin + secondSize &&
           secondBegin < firstBegin + firstSize;
}

int runTest()
{
    PageCache& pageCache = PageCache::getInstance();

    void* eightPages = pageCache.allocateSpan(8);
    if (eightPages == nullptr) {
        std::cerr << "FAIL: could not allocate the initial 8-page span.\n";
        return 1;
    }
    pageCache.deallocateSpan(eightPages, 8);

    void* sixPages = pageCache.allocateSpan(6);
    if (sixPages == nullptr) {
        std::cerr << "FAIL: could not allocate the 6-page span.\n";
        return 1;
    }
    if (sixPages != eightPages) {
        std::cerr << "FAIL: the 6-page allocation did not reuse the 8-page span.\n";
        return 1;
    }

    void* twoPages = pageCache.allocateSpan(2);
    if (twoPages == nullptr) {
        std::cerr << "FAIL: could not allocate the remaining 2-page span.\n";
        return 1;
    }
    if (twoPages != static_cast<char*>(sixPages) + 6 * PageCache::PAGE_SIZE) {
        std::cerr << "FAIL: the 2-page allocation is not the expected right-hand span.\n";
        return 1;
    }

    // 保持 twoPages 在用；归还左侧 Span 时会检查右邻地址。
    pageCache.deallocateSpan(sixPages, 6);

    void* result = pageCache.allocateSpan(2);
    if (result == nullptr) {
        std::cerr << "FAIL: allocation after releasing the 6-page span returned null.\n";
        return 1;
    }

    const size_t twoPageBytes = 2 * PageCache::PAGE_SIZE;
    if (rangesOverlap(result, twoPageBytes, twoPages, twoPageBytes)) {
        std::cerr << "FAIL: the new allocation overlaps the still-in-use 2-page span.\n";
        return 1;
    }

    std::cout << "PASS: allocation returned valid memory without overlapping the live span.\n";
    return 0;
}

} // namespace

int main()
{
    const pid_t child = fork();
    if (child < 0) {
        std::cerr << "FAIL: fork failed.\n";
        return 1;
    }

    if (child == 0) {
        return runTest();
    }

    int status = 0;
    const pid_t waited = waitpid(child, &status, 0);
    if (waited != child) {
        std::cerr << "FAIL: waitpid failed.\n";
        return 1;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        if (WIFSIGNALED(status)) {
            std::cerr << "FAIL: child terminated by signal "
                      << WTERMSIG(status) << ".\n";
        } else {
            std::cerr << "FAIL: child did not exit successfully.\n";
        }
        return 1;
    }

    std::cout << "PageCache empty free-list test passed.\n";
    return 0;
}
