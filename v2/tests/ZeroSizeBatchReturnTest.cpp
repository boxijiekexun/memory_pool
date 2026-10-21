#include "../include/MemoryPool.h"

#include <iostream>
#include <set>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

using namespace Kama_memoryPool;

namespace {

int runTest()
{
    if (SizeClass::roundUp(0) != ALIGNMENT ||
        SizeClass::roundUp(1) != ALIGNMENT ||
        SizeClass::roundUp(ALIGNMENT + 1) != 2 * ALIGNMENT) {
        std::cerr << "zero-size normalization changed another size class boundary\n";
        return 1;
    }

    constexpr size_t blockCount = 300;
    std::vector<void*> original;
    std::set<void*> uniqueOriginal;
    original.reserve(blockCount);

    for (size_t i = 0; i < blockCount; ++i) {
        void* p = MemoryPool::allocate(0);
        if (!p || !uniqueOriginal.insert(p).second) {
            std::cerr << "zero-size allocations failed or returned duplicate blocks\n";
            return 1;
        }
        original.push_back(p);
    }

    // 超过 ThreadCache 的 256 块阈值，确保触发批量归还。
    for (void* p : original) MemoryPool::deallocate(p, 0);

    std::set<void*> reused;
    for (size_t i = 0; i < blockCount; ++i) {
        void* p = MemoryPool::allocate(0);
        if (!p || !reused.insert(p).second || uniqueOriginal.count(p) == 0) {
            std::cerr << "zero-size batch return lost blocks instead of making them reusable\n";
            return 1;
        }
    }

    if (reused.size() != blockCount) {
        std::cerr << "not all zero-size blocks were reused\n";
        return 1;
    }
    std::cout << "PASS: zero-size blocks survive batch return and are reused.\n";
    return 0;
}

} // namespace

int main()
{
    const pid_t child = fork();
    if (child < 0) return 1;
    if (child == 0) _exit(runTest());

    int status = 0;
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        std::cerr << "zero-size batch return regression test failed\n";
        return 1;
    }
    std::cout << "Zero-size batch return test passed.\n";
    return 0;
}
