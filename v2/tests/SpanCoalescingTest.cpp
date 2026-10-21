/*
 * 独立回归测试 PageCache 对相邻空闲 Span 的合并：
 * 申请并归还 8 页后再申请 6 页，让剩余的 2 页继续留在 freeSpans_ 中；
 * 不申请这 2 页，直接归还 6 页并请求完整的 8 页。成功时应复用原地址。
 * 测试在子进程中运行，避免单例 PageCache 的状态影响其他测试。
 */
#include "../include/PageCache.h"

#include <iostream>
#include <sys/wait.h>
#include <unistd.h>

using namespace Kama_memoryPool;

namespace {

int runTest()
{
    auto& pc = PageCache::getInstance();

    void* original = pc.allocateSpan(8);
    if (original == nullptr) {
        std::cerr << "allocateSpan(8) failed for the original span\n";
        return 1;
    }
    pc.deallocateSpan(original, 8);

    // 从原来的 8 页 Span 中取走左侧 6 页。
    void* left = pc.allocateSpan(6);
    if (left == nullptr) {
        std::cerr << "allocateSpan(6) failed\n";
        return 1;
    }
    if (left != original) {
        std::cerr << "allocateSpan(6) did not reuse the original span\n";
        return 1;
    }

    // 剩余的 2 页仍在 freeSpans_ 中；不要申请它，以免掩盖映射遗漏。
    pc.deallocateSpan(left, 6);

    // 合并后应能重新取得完整的 8 页原区域。
    void* merged = pc.allocateSpan(8);
    if (merged == nullptr) {
        std::cerr << "allocateSpan(8) failed after returning the 6-page span\n";
        return 1;
    }
    if (merged != original) {
        std::cerr << "Span coalescing failed\n";
        return 1;
    }

    std::cout << "PASS: adjacent spans coalesced and the original 8-page span was reused.\n";
    return 0;
}

} // namespace

int main()
{
    const pid_t child = fork();
    if (child < 0) {
        std::cerr << "fork failed\n";
        return 1;
    }

    if (child == 0) {
        return runTest();
    }

    int status = 0;
    const pid_t waited = waitpid(child, &status, 0);
    if (waited != child) {
        std::cerr << "waitpid failed\n";
        return 1;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        if (WIFSIGNALED(status)) {
            std::cerr << "child terminated by signal " << WTERMSIG(status) << '\n';
        } else {
            std::cerr << "child did not exit successfully\n";
        }
        return 1;
    }

    std::cout << "PageCache span coalescing test passed.\n";
    return 0;
}
