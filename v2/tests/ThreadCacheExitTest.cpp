#include "../include/MemoryPool.h"
#include "../include/CentralCache.h"
#include "../include/PageCache.h"

#include <array>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

using namespace Kama_memoryPool;

namespace {

int runTest()
{
    constexpr size_t threadRounds = 4;
    constexpr size_t threadsPerRound = 4;
    constexpr size_t returnedBlocksPerThread = 3;
    constexpr std::array<size_t, 3> sizes{{16, 64, 4096}};
    std::array<std::vector<void*>, threadRounds * threadsPerRound> released;
    std::array<std::vector<void*>, threadRounds * threadsPerRound> guards;
    std::array<std::vector<void*>, sizes.size()> mainHeld;
    std::set<void*> allHeld;
    for (size_t round = 0; round < threadRounds; ++round) {
        std::vector<std::thread> workers;
        std::mutex barrierMutex;
        std::condition_variable barrier;
        size_t ready = 0;
        for (size_t worker = 0; worker < threadsPerRound; ++worker) {
            const size_t id = round * threadsPerRound + worker;
            workers.emplace_back([&, id] {
                std::array<std::vector<void*>, sizes.size()> allocations;
                for (size_t classIndex = 0; classIndex < sizes.size(); ++classIndex) {
                    auto& list = allocations[classIndex];
                    const size_t blocksPerBatch = std::min(
                        CentralCache::BATCH_SIZE,
                        8 * PageCache::PAGE_SIZE / sizes[classIndex]);
                    list.reserve(blocksPerBatch);
                    for (size_t i = 0; i < blocksPerBatch; ++i) {
                        void* p = MemoryPool::allocate(sizes[classIndex]);
                        if (!p) std::abort();
                        list.push_back(p);
                    }
                }
                // Keep one live block per worker and size class. This prevents
                // whole-Span reclamation from changing its block partitioning.
                for (size_t classIndex = 0; classIndex < sizes.size(); ++classIndex) {
                    auto& list = allocations[classIndex];
                    for (size_t i = returnedBlocksPerThread; i < list.size(); ++i)
                        guards[id].push_back(list[i]);
                    for (size_t i = 0; i < returnedBlocksPerThread; ++i)
                        released[id].push_back(allocations[classIndex][i]);
                }
                {
                    std::unique_lock<std::mutex> lock(barrierMutex);
                    if (++ready == threadsPerRound) barrier.notify_all();
                    else barrier.wait(lock, [&] { return ready == threadsPerRound; });
                }
                for (size_t classIndex = 0; classIndex < sizes.size(); ++classIndex) {
                    for (size_t i = 0; i < returnedBlocksPerThread; ++i)
                        MemoryPool::deallocate(allocations[classIndex][i], sizes[classIndex]);
                }
                // Worker exits here; its TLS cache must return all retained blocks.
            });
        }
        for (auto& worker : workers) worker.join();

        // 同一轮中的块在 barrier 释放前同时处于用户所有权下，因此地址应唯一。
        for (size_t classIndex = 0; classIndex < sizes.size(); ++classIndex) {
            const size_t expectedCount = threadsPerRound * returnedBlocksPerThread;
            std::set<void*> expected;
            for (size_t worker = 0; worker < threadsPerRound; ++worker) {
                const auto& perThread = released[round * threadsPerRound + worker];
                const size_t begin = classIndex * returnedBlocksPerThread;
                for (size_t i = 0; i < returnedBlocksPerThread; ++i)
                    expected.insert(perThread[begin + i]);
            }
            if (expected.size() != expectedCount) {
                std::cerr << "concurrent workers received duplicate live blocks\n";
                return 1;
            }

            std::set<void*> observed;
            const size_t batchSize = std::min(
                CentralCache::BATCH_SIZE,
                8 * PageCache::PAGE_SIZE / sizes[classIndex]);
            const size_t drainCount = batchSize > expectedCount
                ? batchSize : expectedCount;
            for (size_t i = 0; i < drainCount; ++i) {
                void* p = MemoryPool::allocate(sizes[classIndex]);
                const bool mustMatchExitedThreadBlock = round == 0 && i < expectedCount;
                if (!p || !observed.insert(p).second || allHeld.count(p) != 0 ||
                    (mustMatchExitedThreadBlock && expected.count(p) == 0)) {
                    std::cerr << "worker-exit block mismatch: round=" << round
                              << " size=" << sizes[classIndex]
                              << " allocation=" << i << " expected=" << expected.size()
                              << " observed=" << observed.size() << " ptr=" << p << '\n';
                    for (size_t worker = 0; worker < threadsPerRound; ++worker) {
                        const auto& perThread = released[round * threadsPerRound + worker];
                        const size_t begin = classIndex * returnedBlocksPerThread;
                        std::cerr << " worker " << worker << ':';
                        for (size_t j = 0; j < returnedBlocksPerThread; ++j)
                            std::cerr << ' ' << perThread[begin + j];
                        std::cerr << '\n';
                    }
                    return 1;
                }
                mainHeld[classIndex].push_back(p);
                allHeld.insert(p);
            }
            for (void* p : expected) {
                if (!observed.count(p)) {
                    std::cerr << "not all blocks from exiting workers became reusable\n";
                    return 1;
                }
            }
            if (observed.size() != drainCount) {
                std::cerr << "not all blocks from exiting workers became reusable\n";
                return 1;
            }
        }
    }

    std::cout << "PASS: worker TLS caches were returned and reused across sizes.\n";
    return 0;
}

} // namespace

int main()
{
    return runTest();
}
