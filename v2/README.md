# Kama Memory Pool v2：架构与实现原理

## 1. 项目概述

Kama Memory Pool v2 是一个使用 C++17 实现的多线程内存池（Memory Pool）项目。

它的目标是：

**提高程序频繁分配和释放小块内存时的效率，尤其是减少多线程之间的锁竞争。**

例如，程序可能频繁执行：

```cpp
void* p = MemoryPool::allocate(32);

// 使用内存

MemoryPool::deallocate(p, 32);
```

每次都交给通用内存分配器处理，可能带来额外的分配管理开销。

因此，v2 尝试通过提前获取内存、缓存空闲块以及分层管理的方式减少这些开销。

注意：通用的 `malloc/free` 本身也有优化和缓存。自定义内存池不一定更快，需要通过性能测试验证。

---

## 2. 从 v1 到 v2：为什么需要重新设计？

### 2.1 v1 的设计

你之前学习的 v1，可以简化成：

```text
用户申请内存
     │
     ▼
  HashBucket
     │
     ▼
选择对应的 MemoryPool
     │
     ├── Free List 有空闲 Slot → 直接返回
     │
     └── 没有可用 Slot
             │
             ▼
         从 Block 分配
             │
             └── Block 不够 → 申请新 Block
```

这种设计比较直观。

但是，如果有 8 个线程同时申请 32 字节，它们就需要访问同一个 32 字节 MemoryPool 的共享分配状态。

虽然 v1 通过 CAS 和 mutex 处理部分并发操作，但多个线程仍然可能竞争同一组共享资源。

### 2.2 v2 的改进思路

v2 提出一个问题：

**能不能让线程先使用自己管理的空闲内存，尽量减少对共享内存池的访问？**

这就产生了三级缓存架构：

- ThreadCache（线程本地缓存）
- CentralCache（中心缓存）
- PageCache（页缓存）

三者分别解决不同层次的问题。

---

## 3. v2 的总体架构

这是理解整个项目最重要的一张图。

```text
                    Application
                         │
                         ▼
                  MemoryPool API
                         │
              ┌──────────┴──────────┐
              ▼                     ▼
          Thread A              Thread B
              │                     │
              ▼                     ▼
       ┌─────────────┐       ┌─────────────┐
       │ ThreadCache │       │ ThreadCache │
       │   Free List │       │   Free List │
       └──────┬──────┘       └──────┬──────┘
              │                     │
              └──────────┬──────────┘
                         ▼
                ┌────────────────┐
                │  CentralCache  │
                │  共享空闲链表   │
                └────────┬───────┘
                         │
                         ▼
                ┌────────────────┐
                │   PageCache    │
                │  管理内存 Span  │
                └────────┬───────┘
                         │
                         ▼
                mmap / OS 虚拟内存管理
```

**这是一个从上到下获取内存、从下到上提供内存的架构。**

用户申请内存时：

1. 首先访问自己的 ThreadCache。
2. 如果本地没有可用内存，访问 CentralCache。
3. 如果 CentralCache 也没有，访问 PageCache。
4. 如果 PageCache 没有合适的 Span，再通过 `mmap` 获取内存。

释放内存时，通常先归还 ThreadCache。当缓存积累太多空闲块时，再逐层向下归还。

不过，大于 256 KiB 的请求在当前 v2 中直接走 `malloc/free`，不经过上述完整缓存路径。

---

## 4. 三层缓存分别做什么？

### 4.1 ThreadCache：线程自己的内存池

ThreadCache 位于最上层，是程序最先访问的缓存。

它的核心特点是：**每个线程拥有自己的 ThreadCache。**

在 `ThreadCache.h` 中：

```cpp
static ThreadCache* getInstance()
{
    static thread_local ThreadCache instance;
    return &instance;
}
```

这里的 `thread_local` 是关键。

假设线程 A 和线程 B 分别调用 `getInstance()`：

```text
Thread A                 Thread B
    │                        │
    ▼                        ▼
ThreadCache A           ThreadCache B
    │                        │
    ▼                        ▼
Free List A             Free List B
```

两个线程获取的是不同的对象。

因此，当线程 A 在自己的缓存中取出一个空闲 Slot 时，一般不需要与线程 B 竞争同一把锁。

这是 v2 减少锁竞争的核心设计。

**ThreadCache 内部存储什么？**

源码中有：

```cpp
std::array<void*, FREE_LIST_SIZE> freeList_;

std::array<size_t, FREE_LIST_SIZE> freeListSize_;
```

第一个数组保存各个大小类的空闲链表头。

第二个数组记录每个大小类的空闲块数量。

例如：

```text
ThreadCache A

freeList_[0]   → 8B  → 8B  → nullptr
freeList_[1]   → 16B → nullptr
freeList_[2]   → nullptr
freeList_[3]   → 32B → 32B → nullptr
...
```

这里的 `freeList_[3]` 不是一个 32 字节的内存块，而是**指向 32 字节空闲块链表的头指针**。

如果程序申请 32 字节，并且 `freeList_[3]` 不为空，就可以直接取走链表头节点。

这种本地缓存命中，是我们最希望发生的快速分配路径。

### 4.2 CentralCache：所有线程共享的缓存

现在考虑：

Thread A 需要 32 字节，但是它自己的 Free List 已经空了。

怎么办？

它就向 CentralCache 请求内存。

CentralCache 是共享的：

```text
ThreadCache A ──┐
                │
ThreadCache B ──┼──→ CentralCache
                │
ThreadCache C ──┘
```

它负责在多个线程的本地缓存与底层 PageCache 之间提供共享的空闲内存资源。

CentralCache 同样按照对象大小维护不同的 Free List。

区别在于：

- ThreadCache 的链表由各个线程独立管理。
- CentralCache 的链表可能被多个线程访问，因此需要同步。

项目使用按大小类划分的自旋锁（Spinlock）来保护中心链表。

**为什么不让线程直接访问 PageCache？**

因为访问更底层的内存管理通常涉及更多元数据操作、锁竞争，甚至新的内存映射。

CentralCache 可以缓存已经切分好的小块，减少这些操作发生的次数。

当前 v2 源码中的 `fetchRange()` 通常一次向线程返回一个小块，并不像一些成熟分配器那样真正执行自适应的批量获取。这是设计目标与当前实现需要区分的地方。

### 4.3 PageCache：管理较大的连续内存

PageCache 是最底层的缓存。

它不负责给用户直接分配 8 字节、16 字节、32 字节的小对象，而是负责获取和管理较大的连续内存区域。

项目中定义：

```cpp
static const size_t PAGE_SIZE = 4096;
```

也就是每个逻辑 Page 为 4096 字节。

多个连续 Page 可以组成一个 Span。

例如：

```text
Page = 4 KiB

一个 Span：

┌──────┬──────┬──────┬──────┐
│ Page │ Page │ Page │ Page │
└──────┴──────┴──────┴──────┘

Span 大小 = 4 × 4 KiB = 16 KiB
```

PageCache 管理的就是这些 Span。

当前 v2 中，CentralCache 对不超过 32 KiB 的大小类，通常尝试向 PageCache 请求一个 8 Page 的 Span。

也就是：

\[
8\times4096=32768\text{ Bytes}
\]

然后 CentralCache 可以把这个 Span 切分成更小的内存块。

比如切分为 32 字节的块：

\[
32768/32=1024
\]

就可以得到 1024 个 32 字节内存块。

PageCache 通过 `mmap` 获取新的虚拟内存区域，同时维护空闲 Span 信息，支持部分 Span 分割和相邻合并。

注意：

这里的 PageCache 与数据库 Buffer Pool 不是一回事。前者管理分配器使用的内存区域；后者主要缓存磁盘上的数据库页面。

---

## 5. 一次真实的内存分配过程

我们现在把三个组件串起来。

假设程序执行：

```cpp
void* ptr = MemoryPool::allocate(25);
```

### 第一步：进入 MemoryPool

在 `MemoryPool.h` 中：

```cpp
static void* allocate(size_t size)
{
    return ThreadCache::getInstance()->allocate(size);
}
```

这里不直接分配内存，而是把请求交给当前线程的 ThreadCache。

### 第二步：确定对象属于哪个大小类

在 `Common.h` 中：

```cpp
constexpr size_t ALIGNMENT = 8;
constexpr size_t MAX_BYTES = 256 * 1024;
constexpr size_t FREE_LIST_SIZE = MAX_BYTES / ALIGNMENT;
```

这意味着项目按照 8 字节为单位划分大小类：

| Size Class | 内存块大小 |
|---|---|
| 0 | 8B |
| 1 | 16B |
| 2 | 24B |
| 3 | 32B |
| 4 | 40B |
| ... | ... |
| 32767 | 256 KiB |

25 字节对应的大小类是：

\[
\left\lceil25/8\right\rceil=4
\]

由于数组下标从 0 开始，所以：

\[
index=4-1=3
\]

因此会使用：

```cpp
freeList_[3]
```

这里存放的都是大小为 32 字节的空闲块。

注意，`ALIGNMENT = 8` 在这里主要定义大小类的分组粒度；它不能保证满足所有 C++ 类型的对齐要求。

### 第三步：检查 ThreadCache

假设：

```text
freeList_[3] → A → B → nullptr
```

那么：

```cpp
void* ptr = freeList_[3];
```

就可以从链表中取出 A。

返回给用户后：

```text
freeList_[3] → B → nullptr
```

这次分配不需要访问 CentralCache，也不需要调用 `mmap`。

### 第四步：如果 ThreadCache 为空

假设：

```text
freeList_[3] → nullptr
```

这时执行：

```cpp
fetchFromCentralCache(index);
```

向 CentralCache 申请 32 字节的空闲块。

如果 CentralCache 中存在：

```text
CentralCache[3] → C → D → E → ...
```

就可以取出一个块，例如 C，返回给 ThreadCache。

### 第五步：如果 CentralCache 也为空

这时 CentralCache 请求 PageCache：

```text
PageCache
    │
    ▼
获得一个 8 Page 的 Span
    │
    ▼
32768 Bytes
    │
    ▼
切分为 1024 个 32B 小块
    │
    ▼
一个返回给上层，其余留在 CentralCache
```

如果 PageCache 中没有可以复用的 Span，就调用底层 `mmap()` 申请新的内存区域。

到这里，一次 25 字节的内存请求就完成了。

这就是 v2 最核心的分配路径。

---

## 6. 内存释放时发生什么？

假设用户执行：

```cpp
MemoryPool::deallocate(ptr, 25);
```

首先根据传入的大小找到对应大小类：

```text
25 Bytes → Size Class 3 → 32 Bytes
```

然后将这块内存加入当前线程的 `freeList_[3]`。

这和 v1 中的 Free List 头插法很相似。

**但是，ThreadCache 不能无限制地保存空闲块。**

如果一个线程不断释放内存，却很少重新申请，那么它的本地缓存就可能积累越来越多的空闲块。

所以 v2 使用一个缓存阈值。

在当前 `ThreadCache.cpp` 中：

```cpp
size_t threshold = 256;
```

当某个大小类的空闲块数量超过阈值，ThreadCache 会尝试保留一部分，把其余部分归还 CentralCache。

CentralCache 又通过 SpanTracker 记录 Span 的状态，并尝试在满足条件时将完整空闲的 Span 归还 PageCache。

整个设计意图是：

```text
用户释放内存
      │
      ▼
 ThreadCache
      │
      │ 空闲块过多
      ▼
 CentralCache
      │
      │ 整个 Span 可以回收
      ▼
   PageCache
```

这里要强调：这些是架构上的回收层次。当前 v2 的回收计数和 Span 管理实现仍有需要验证的正确性问题。

并且当前 PageCache 没有实现通过 `munmap()` 将空闲 Span 真正归还操作系统的完整路径。

因此，**释放给内存池，不等于立刻将内存还给操作系统。**

---

## 7. 对应到实际源码：每个文件负责什么？

项目结构：

```text
v2/
├── CMakeLists.txt
├── include/
│   ├── Common.h
│   ├── MemoryPool.h
│   ├── ThreadCache.h
│   ├── CentralCache.h
│   └── PageCache.h
├── src/
│   ├── ThreadCache.cpp
│   ├── CentralCache.cpp
│   └── PageCache.cpp
└── tests/
    ├── UnitTest.cpp
    └── PerformanceTest.cpp
```

各文件的责任如下：

| 文件 | 主要职责 |
|---|---|
| `Common.h` | 定义大小类、基础常量和公共数据结构 |
| `MemoryPool.h` | 提供对外 allocate/deallocate 接口 |
| `ThreadCache.h/.cpp` | 线程本地 Free List、分配和回收 |
| `CentralCache.h/.cpp` | 共享 Free List、锁、SpanTracker |
| `PageCache.h/.cpp` | Span 管理、分割、合并和 mmap |
| `UnitTest.cpp` | 基本正确性测试 |
| `PerformanceTest.cpp` | 与普通内存分配方式进行性能对比 |

三个非常重要的 C++ 知识点：

**thread_local**

每个线程拥有独立的 ThreadCache 实例。

**Singleton（单例模式）**

CentralCache 和 PageCache 通过 `getInstance()` 提供进程内共享的对象。

**Atomic / Mutex / Spinlock**

用于保护共享数据结构，并在性能与同步开销之间进行权衡。

---

## 8. v2 的设计代价与目前存在的问题

三级缓存不是没有成本。

### 8.1 内存开销

因为：

```cpp
FREE_LIST_SIZE = 32768;
```

在常见 64 位环境下，ThreadCache 中两个大小为 32768 的数组，就需要约 512 KiB 的存储空间。

如果有很多线程，ThreadCache 自身的内存开销就会比较明显。

### 8.2 内部碎片

用户申请 25 字节，但实际分配 32 字节。

多出的 7 字节在这次分配中无法供其他对象使用。

### 8.3 缓存分布不均

可能出现：

```text
Thread A：空闲内存很多
Thread B：几乎没有空闲内存
```

由于 ThreadCache 是线程独立的，线程 B 不能直接把线程 A 的本地链表当作自己的链表使用。

需要通过中心缓存机制协调。

### 8.4 原项目仍有正确性问题值得检查

静态阅读源码，可以发现一些需要后续测试验证的点：

- `ThreadCache::allocate()` 在检查链表是否为空之前，就减少了空闲块计数，存在无符号下溢和计数维护问题。
- 小块 `deallocate()` 路径没有先检查空指针。
- 不同大小类统一按 8 字节分组，并不能满足所有过度对齐类型的要求。
- Span 的空闲数量统计和回收条件需要更严格的验证。
- 部分内存回收策略尚未完整实现。

这些问题意味着 v2 更适合作为学习项目，而不是直接当作生产级分配器使用。

不过，它们也是以后开发自己的 v4 时很好的改进方向。

---

## 9. 推荐的源码学习顺序

建议按照一次内存请求的执行路径阅读，而不是按文件名从头到尾逐行阅读。

**第一阶段：理解分配流程**

1. `MemoryPool.h`：找到入口。
2. `Common.h`：理解大小类映射。
3. `ThreadCache.h`：理解 `thread_local` 和 Free List 数组。
4. `ThreadCache.cpp::allocate()`：理解本地分配。
5. `CentralCache.cpp::fetchRange()`：理解共享缓存。
6. `PageCache.cpp::allocateSpan()`：理解 Span 从哪里来。

**第二阶段：理解释放流程**

从 `ThreadCache::deallocate()` 开始，沿着 `returnToCentralCache()`、`CentralCache::returnRange()` 继续向下追踪。

**第三阶段：理解并发与优化**

最后再研究自旋锁、原子操作、SpanTracker、延迟归还机制以及 Benchmark。

这样可以先建立整体调用关系，再进入复杂细节。

---

## 10. 最后总结

v2 最核心的设计思想可以用三句话概括：

**ThreadCache：** 尽量在当前线程内部完成分配，避免锁竞争。

**CentralCache：** 维护线程之间共享的空闲内存块，为 ThreadCache 提供补充。

**PageCache：** 管理较大连续内存区域，并在需要时向底层申请新的内存。

用户申请内存时，通常沿着：

`ThreadCache → CentralCache → PageCache → mmap`

寻找可用空间。

这个架构最重要的并不是某一个具体函数，而是**通过分层缓存，让大部分高频分配发生在成本较低、竞争较少的位置**。

