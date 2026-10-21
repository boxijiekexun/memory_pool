/*
 * 回归测试 PageCache 分割 Span 时的 spanMap_ 登记：
 * 申请并归还 8 页后申请 6 页，使剩余 2 页留在空闲链表；不申请这 2 页，
 * 直接归还 6 页并重新申请 8 页。最终地址必须复用原来的 8 页区域。
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

    void* p = pc.allocateSpan(8);
    if (p == nullptr) {
        std::cerr << "FAIL: allocateSpan(8) returned null initially.\n";
        return 1;
    }
    // 把原始 Span 放回缓存，后续 6 页申请应从它切分。
    pc.deallocateSpan(p, 8);

    void* a = pc.allocateSpan(6);
    if (a == nullptr) {
        std::cerr << "FAIL: allocateSpan(6) returned null.\n";
        return 1;
    }
    if (a != p) {
        std::cerr << "FAIL: allocateSpan(6) did not reuse the original span.\n";
        return 1;
    }

    // 故意不申请剩余的 2 页：该尾部 Span 必须已在 spanMap_ 登记。
    pc.deallocateSpan(a, 6);

    // 完整 8 页请求只能通过识别并合并尾部空闲 Span 来满足。
    void* result = pc.allocateSpan(8);
    if (result == nullptr) {
        std::cerr << "FAIL: allocateSpan(8) returned null after coalescing.\n";
        return 1;
    }
    if (result != p) {
        std::cerr << "FAIL: allocateSpan(8) did not reuse the coalesced original span.\n";
        return 1;
    }

    std::cout << "PASS: the original 8-page span was coalesced and reused.\n";
    return 0;
}

} // namespace

int main()
{
    // 在独立子进程中使用 PageCache 单例，避免状态影响其他测试。
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

    std::cout << "PageCache span mapping test passed.\n";
    return 0;
}
