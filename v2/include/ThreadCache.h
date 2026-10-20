#pragma once
#include "Common.h"

namespace Kama_memoryPool 
{

// 线程本地缓存
class ThreadCache
{
public:
  static ThreadCache *getInstance() {
        // thread_local 关键字用于声明线程局部存储（Thread Local
        // Storage，TLS）变量。 每个线程都会有自己独立的实例，互不干扰。
        // Thread Local Storage（TLS，线程局部存储）
        static thread_local ThreadCache instance;
        return &instance;
    }

    void* allocate(size_t size);
    void deallocate(void* ptr, size_t size);
private:
    ThreadCache() 
    {
        // 初始化自由链表和大小统计
        freeList_.fill(nullptr);
        freeListSize_.fill(0);
    }
    
    // 从中心缓存获取内存
    void* fetchFromCentralCache(size_t index);
    // 归还内存到中心缓存
    void returnToCentralCache(void* start, size_t size);
    // 判断是否需要将部分内存回收给中心缓存
    bool shouldReturnToCentralCache(size_t index);
private:
    // 每个线程的自由链表数组
    std::array<void*, FREE_LIST_SIZE>  freeList_; // 自由链表数组，存储每个大小类的空闲内存块
    std::array<size_t, FREE_LIST_SIZE> freeListSize_; // 自由链表大小统计   
};

} // namespace memoryPool