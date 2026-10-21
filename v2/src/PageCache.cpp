#include "PageCache.h"
#include <sys/mman.h>
#include <cstring>

namespace Kama_memoryPool
{

// 查找、分割或申请新的 Span
void* PageCache::allocateSpan(size_t numPages)
{
    std::lock_guard<std::mutex> lock(mutex_);

    // 查找合适的空闲span
    // freeSpans_是 map<size_t, Span*>，key是span的页数，value是对应的Span*链表头指针
    // lower_bound函数返回第一个大于等于numPages的元素的迭代器
    auto it = freeSpans_.lower_bound(numPages);
    if (it != freeSpans_.end())
    {
        Span* span = it->second;

        // 将取出的span从原有的空闲链表freeSpans_[it->first]中移除
        if (span->next) {
            // span->next不为空，说明链表中还有其他span，将链表头指针更新为下一个span
            freeSpans_[it->first] = span->next;
        }
        else
        {
            freeSpans_.erase(it);
        }
        // 已从空闲链表摘除，当前正在分配。
        span->isFree = false;

        // 如果span大于需要的numPages则进行分割
        if (span->numPages > numPages) 
        {
            Span* newSpan = new Span;
            newSpan->pageAddr = static_cast<char*>(span->pageAddr) + 
                                numPages * PAGE_SIZE;
            newSpan->numPages = span->numPages - numPages;
            newSpan->next = nullptr;
            newSpan->isFree = true;

            // 将超出部分放回空闲Span*列表头部
            auto& list = freeSpans_[newSpan->numPages];
            newSpan->next = list;
            list = newSpan;

            // 记录这个剩余 Span 的起始地址
            spanMap_[newSpan->pageAddr] = newSpan;
            span->numPages = numPages;
        }

        // 记录span信息用于回收
        spanMap_[span->pageAddr] = span;
        return span->pageAddr;
    }

    // 没有合适的span，向系统申请
    void* memory = systemAlloc(numPages);
    if (!memory) return nullptr;

    // 创建新的span
    Span* span = new Span;
    span->pageAddr = memory;
    span->numPages = numPages;
    span->next = nullptr;

    // 记录span信息用于回收
    spanMap_[memory] = span;
    return memory;
}

// 回收和合并空闲 Span
void PageCache::deallocateSpan(void* ptr, size_t numPages)
{
    std::lock_guard<std::mutex> lock(mutex_);

    // 查找对应的span，没找到代表不是PageCache分配的内存，直接返回
    auto it = spanMap_.find(ptr);
    if (it == spanMap_.end()) return;

    Span* span = it->second;

    // 阻止同一 Span 被重复归还并重复插入空闲链表。
    if (span->isFree) return;

    // 从对应大小的空闲链表中摘除一个 Span。
    auto removeFreeSpan = [this](Span* target) {
        auto bucket = freeSpans_.find(target->numPages);
        if (bucket == freeSpans_.end()) return false;

        Span*& head = bucket->second;
        if (head == target) {
            head = target->next;
        } else {
            Span* prev = head;
            while (prev && prev->next != target) prev = prev->next;
            if (!prev) return false;
            prev->next = target->next;
        }

        if (!head) freeSpans_.erase(bucket);
        target->isFree = false;
        target->next = nullptr;
        return true;
    };

    // 先查有序地址映射中的前驱。只有前驱的末尾恰好等于当前起始地址时才相邻。
    auto currentIt = spanMap_.find(ptr);
    if (currentIt != spanMap_.begin()) {
        auto prevIt = currentIt;
        --prevIt;
        Span* prevSpan = prevIt->second;
        void* prevEnd = static_cast<char*>(prevSpan->pageAddr) + prevSpan->numPages * PAGE_SIZE;
        if (prevEnd == ptr && prevSpan->isFree && removeFreeSpan(prevSpan)) {
            prevSpan->numPages += span->numPages;
            spanMap_.erase(currentIt);
            delete span;
            span = prevSpan;
        }
    }

    // 检查合并后 Span 右侧的相邻 Span。使用元数据中的实际页数计算边界。
    void* nextAddr = static_cast<char*>(span->pageAddr) + span->numPages * PAGE_SIZE;
    auto nextIt = spanMap_.find(nextAddr);
    if (nextIt != spanMap_.end()) {
        Span* nextSpan = nextIt->second;
        if (nextSpan->isFree && removeFreeSpan(nextSpan)) {
            span->numPages += nextSpan->numPages;
            spanMap_.erase(nextIt);
            delete nextSpan;
        }
    }

    // 将合并后的span通过头插法插入空闲列表
    auto& list = freeSpans_[span->numPages];
    span->next = list;
    list = span;
    span->isFree = true;
    
}

void* PageCache::systemAlloc(size_t numPages)
{
    size_t size = numPages * PAGE_SIZE;

    // 使用mmap分配内存
    void* ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ptr == MAP_FAILED) return nullptr;

    // 清零内存
    memset(ptr, 0, size);
    return ptr;
}

} // namespace memoryPool
