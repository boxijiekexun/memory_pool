/*
 * 测试思路：
 * 1. 申请 8 页 Span 并归还，使 PageCache 中有一个 8 页空闲 Span。
 * 2. 申请 6 页，触发分割，剩余的 2 页 Span 放入 2 页空闲链表。
 * 3. 再申请 2 页，取走剩余 Span；此时 6 页和 2 页 Span 都在使用。
 * 4. 仅归还 6 页 Span。释放逻辑检查右侧 2 页 Span 时，会通过
 *    freeSpans_[2] 取得空链表头，并可能在 map 中留下一个空链表项。
 * 5. 再申请 2 页，检查是否因取到空链表项而解引用空指针。
 *
 * 当前测试在子进程中执行上述步骤，父进程检查子进程是否因最后一次
 * 申请触发 SIGSEGV，避免崩溃终止整个测试进程。
 */
#include "../include/PageCache.h"

#include <cassert>
#include <csignal>
#include <iostream>
#include <sys/wait.h>
#include <unistd.h>

using namespace Kama_memoryPool;


// Bug reproducer:
// Success means the child crashed with SIGSEGV.
// This is NOT yet a regression test.



int main() {
    // 在子进程中运行测试，以便在发生段错误时不会终止整个测试套件
    const pid_t child = fork();
    assert(child >= 0);

    if (child == 0)
    {
        PageCache& pageCache = PageCache::getInstance();

        void* eightPages = pageCache.allocateSpan(8);
        assert(eightPages != nullptr);
        pageCache.deallocateSpan(eightPages, 8);

        void* sixPages = pageCache.allocateSpan(6);
        assert(sixPages == eightPages);

        void* twoPages = pageCache.allocateSpan(2);
        assert(twoPages == static_cast<char*>(sixPages) + 6 * PageCache::PAGE_SIZE);

        // 右侧 2 页 Span 仍在使用；释放 6 页 Span 时会检查这个相邻位置。
        pageCache.deallocateSpan(sixPages, 6);

        // 若空链表项仍存在，lower_bound(2) 会取到空的 Span* 链表头并解引用。
        (void)pageCache.allocateSpan(2);
        _exit(0);
    }

    int status = 0;
    assert(waitpid(child, &status, 0) == child);

    // 当前测试用于复现问题：只有子进程在最后一次申请时收到 SIGSEGV 才算通过。
    // 实现修复后，应改为检查子进程正常退出且最后一次申请成功。
    if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGSEGV)
    {
        std::cerr << "预期空链表场景下 allocateSpan(2) 触发 SIGSEGV\n";
        return 1;
    }

    std::cout << "已复现空闲链表头空指针解引用（SIGSEGV）。\n";
    return 0;
}
