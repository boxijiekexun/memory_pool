#include "../include/PageCache.h"

#include <iostream>
#include <sys/wait.h>
#include <unistd.h>

using namespace Kama_memoryPool;

namespace {

int runTest()
{
    auto& pc = PageCache::getInstance();

    void* p = pc.allocateSpan(1);
    if (p == nullptr) {
        std::cerr << "allocateSpan(1) failed for the original span\n";
        return 1;
    }

    pc.deallocateSpan(p, 1);
    pc.deallocateSpan(p, 1); // 重复释放应被忽略

    void* a = pc.allocateSpan(1);
    void* b = pc.allocateSpan(1);
    if (a == nullptr || b == nullptr) {
        std::cerr << "allocateSpan(1) failed after the repeated deallocation\n";
        return 1;
    }
    if (a == b) {
        std::cerr << "double free caused the same page to be allocated twice\n";
        return 1;
    }

    std::cout << "PASS: double free was ignored and allocations are distinct.\n";
    return 0;
}

} // namespace

int main()
{
    // 子进程隔离 PageCache 单例状态。
    const pid_t child = fork();
    if (child < 0) {
        std::cerr << "fork failed\n";
        return 1;
    }
    if (child == 0) return runTest();

    int status = 0;
    if (waitpid(child, &status, 0) != child) {
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

    std::cout << "PageCache double-free test passed.\n";
    return 0;
}
