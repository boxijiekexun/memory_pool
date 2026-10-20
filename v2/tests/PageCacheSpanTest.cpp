/*
 * 回归测试 PageCache 对相邻空闲 Span 的合并：
 * 先申请并归还 8 页 Span，再分配为左侧 6 页和右侧 2 页；按右、左
 * 顺序归还后，左侧 Span 应与右侧 Span 合并。最后申请 2 页并检查
 * 返回地址来自合并后的连续区域，避免仅因 mmap 申请新内存而误判通过。
 */
#include "../include/PageCache.h"

#include <cassert>
#include <csignal>
#include <iostream>
#include <sys/wait.h>
#include <unistd.h>
using namespace Kama_memoryPool;

int main() {
    std::cout << "Starting PageCache adjacent-span merge test..." << std::endl;
    const pid_t child = fork();
    assert(child >= 0);

    if (child == 0) {
        // Child process
        auto& pc = PageCache::getInstance();

        void* p = pc.allocateSpan(8);
        if (p == nullptr) {
            std::cerr << "FAIL: could not allocate the initial 8-page span.\n";
            return 1;
        }
        pc.deallocateSpan(p, 8);
        std::cout << "PASS: allocated and released an 8-page span.\n";

        void* a = pc.allocateSpan(6);
        void* b = pc.allocateSpan(2);
        if (a == nullptr || b == nullptr) {
            std::cerr << "FAIL: could not split the span into 6 and 2 pages.\n";
            return 1;
        }
        if (a != p || b != static_cast<char*>(a) + 6 * PageCache::PAGE_SIZE) {
            std::cerr << "FAIL: allocations did not come from the expected adjacent regions.\n";
            return 1;
        }
        std::cout << "PASS: split the span into adjacent 6-page and 2-page regions.\n";

        // 先释放右侧 Span
        pc.deallocateSpan(b, 2);
        std::cout << "PASS: released the right 2-page span.\n";

        // 再释放左侧 Span，触发合并
        pc.deallocateSpan(a, 6);
        std::cout << "PASS: released the left 6-page span to trigger merging.\n";

        // 再次申请 2 Page
        void *c = pc.allocateSpan(2);

        // 必须复用合并后的 Span；仅检查非空可能让重新 mmap 掩盖合并失败。
        if (c != a) {
            std::cerr << "FAIL: allocation did not reuse the merged span.\n";
            return 1;
        }
        std::cout << "PASS: reused the merged span for a 2-page allocation.\n";
        return 0;
    }
    // Parent process
    int status = 0;

    if (waitpid(child, &status, 0) != child) {
        std::cerr << "waitpid failed\n";
        return 1;
    }

    if (WIFSIGNALED(status)) {
        std::cerr << "Child terminated by signal: "
                << WTERMSIG(status) << '\n';
        return 1;
    }

    if (!WIFEXITED(status)) {
        return 1;
    }
    
    if (WEXITSTATUS(status) != 0) {
        std::cerr << "PageCache adjacent-span merge test failed.\n";
        return WEXITSTATUS(status);
    }

    std::cout << "PageCache adjacent-span merge test passed.\n";
    return 0;
}
