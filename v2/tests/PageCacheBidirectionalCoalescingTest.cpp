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
    if (!original) {
        std::cerr << "allocateSpan(8) failed\n";
        return 1;
    }
    pc.deallocateSpan(original, 8);

    // 切成 3 + 2 + 3 页，并保持右侧 3 页仍被占用。
    void* left = pc.allocateSpan(3);
    void* middle = pc.allocateSpan(2);
    void* right = pc.allocateSpan(3);
    if (!left || !middle || !right || left != original ||
        static_cast<char*>(middle) != static_cast<char*>(left) + 3 * PageCache::PAGE_SIZE ||
        static_cast<char*>(right) != static_cast<char*>(middle) + 2 * PageCache::PAGE_SIZE) {
        std::cerr << "failed to split the original span into adjacent pieces\n";
        return 1;
    }

    pc.deallocateSpan(left, 3);
    pc.deallocateSpan(middle, 2); // 应向左合并 left；右侧 right 仍在使用
    pc.deallocateSpan(right, 3);  // 再向左合并，恢复完整 8 页

    void* merged = pc.allocateSpan(8);
    if (merged != original) {
        std::cerr << "bidirectional coalescing did not restore the original span\n";
        return 1;
    }

    std::cout << "PASS: adjacent spans coalesced from both directions.\n";
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
    if (child == 0) return runTest();

    int status = 0;
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        std::cerr << "PageCache bidirectional coalescing test failed\n";
        return 1;
    }
    std::cout << "PageCache bidirectional coalescing test passed.\n";
    return 0;
}
