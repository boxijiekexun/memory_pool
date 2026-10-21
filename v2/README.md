# Memory Pool v2：设计与实现

v2 是在v1基础上重构的内存池实现。
为什么会产生这个想法呢？
主要是v1在多线程环境下存在一些问题，v2的目标是解决这些问题并提供更好的性能和可维护性。

v2 通过三重缓存结构（ThreadCache、CentralCache、PageCache）来管理内存分配和释放。每个线程拥有自己的 ThreadCache，减少了锁的竞争；CentralCache 作为共享缓存，协调不同线程之间的内存使用；PageCache 管理实际的内存页和 Span。

对外入口位于 [`include/MemoryPool.h`](include/MemoryPool.h)：

```cpp
#include "MemoryPool.h"

void* p = Kama_memoryPool::MemoryPool::allocate(25);
// 使用 p
Kama_memoryPool::MemoryPool::deallocate(p, 25);
```

释放时需要传入与分配时相同的请求大小，以便定位大小类。`allocate(0)` 按至少一个 `ALIGNMENT` 单位处理；`deallocate(nullptr, size)` 不做操作。

## 整体架构

```text
Application
    ↓ allocate / deallocate
ThreadCache（每线程）
    ↓ 批量 Block 转移
CentralCache（线程共享）
    ↓ Span 分配 / 回收
PageCache（管理 Page 与 Span）
    ↓ 需要新内存时
mmap
```

分配从上向下逐层寻找可用内存：ThreadCache 本地命中时不访问共享缓存；本地缺块时向 CentralCache 批量取块；CentralCache 也没有块时向 PageCache 申请 Span。释放通常先进入当前线程的缓存，之后可能批量转给 CentralCache；当某个 Span 的所有 Block 都回到 CentralCache 时，该 Span 才能交还 PageCache。

超过 `MAX_BYTES`（256 KiB）的请求由 ThreadCache 直接调用 `std::malloc`，释放时调用 `std::free`。这仍由 C/C++ 运行库分配器管理，不代表每次调用都会进入内核或触发系统调用。

## 大小类

[`include/Common.h`](include/Common.h) 定义：

```cpp
ALIGNMENT = alignof(std::max_align_t)
MAX_BYTES = 256 * 1024
FREE_LIST_SIZE = MAX_BYTES / ALIGNMENT
```

`ALIGNMENT` 由目标平台的 `std::max_align_t` 决定，不是固定的 8 字节。请求大小向上取整到 `ALIGNMENT` 的整数倍，并据此选择从 0 开始的大小类索引。零字节请求被归一到最小大小类，因此分配和释放会选择相同的链表。

例如，若某平台 `ALIGNMENT` 为 16，25 字节请求会进入 32 字节大小类，索引为 `32 / 16 - 1 == 1`。具体值应以当前平台编译得到的常量为准。ThreadCache 为每个大小类各保存一个链表头和一个计数；其数组存储开销约为：

```text
FREE_LIST_SIZE × (sizeof(void*) + sizeof(size_t))
```

因此，线程数量较多时，本地缓存元数据也会产生显著开销。

## ThreadCache

[`include/ThreadCache.h`](include/ThreadCache.h) 通过函数内的 `static thread_local` 对象为每个使用该接口的线程建立独立 ThreadCache。各线程的本地 Free List 不由其他线程直接访问。

### 分配与批量获取

1. 根据请求大小计算大小类索引。
2. 本地链表非空时摘下一个 Block 并返回。
3. 本地链表为空时调用 `CentralCache::fetchRange()`。
4. CentralCache 最多交给 ThreadCache 32 个 Block；ThreadCache 将第一个返回给调用方，其余节点保存在本地链表。

`freeListSize_[index]` 表示当前仍由 ThreadCache 空闲链表持有的节点数，不包括刚返回给调用方的 Block。分配本地节点时计数减一；批量取块时按实际留在本地链表的节点数更新。

### 释放与批量归还

小块释放时，Block 被头插到当前线程对应大小类的本地链表，计数加一。链表数量超过 256 后，ThreadCache 保留约四分之一（至少一个节点），其余节点组成链表批量归还 CentralCache。传输时会断开保留部分与归还部分，并同步更新本地计数。

ThreadCache 析构时会遍历各大小类，把仍留在本地的空闲链表交给 CentralCache。CentralCache 和 PageCache 单例采用进程生命周期的动态分配，避免线程局部对象析构时访问已经销毁的单例；它们不会在进程退出前主动析构回收。

本地重复释放检查只遍历**当前线程、当前大小类的本地空闲链表**。它可以阻止同一节点重复插入该链表，但不能作为通用的指针校验或跨线程重复释放检测机制；已转入 CentralCache 的块也不在该检查范围内。调用者仍须遵守正确的分配/释放配对规则。

## CentralCache

[`include/CentralCache.h`](include/CentralCache.h) 为每个大小类维护共享 Free List、一个自旋锁和一组 `SpanTracker`。`atomic_flag` 锁在对应链表和追踪元数据操作期间串行化访问；竞争时线程循环尝试并让出执行机会。

### 批量获取

`fetchRange(index)` 最多从中央链表摘取 `BATCH_SIZE == 32` 个节点。批次边界确定后，函数按每个节点所属的 Span 汇总数量、校验追踪记录和计数，再更新 `freeCount`、中央链表头和批次尾指针。返回的链表与中央链表断开，交给 ThreadCache 管理。

中央链表为空时，CentralCache 向 PageCache 申请 Span，将其切成固定大小的 Block：最多 32 个交给 ThreadCache，其余留在 CentralCache。大 Block 类所需页数按 Block 大小计算。

### 批量归还和 SpanTracker

`returnRange(start, size, index)` 根据大小类和字节数确定应归还的节点数量，遍历待归还链表，核对节点对应的 Span，并统计每个 Span 本批次归还的数量。计数检查通过后，才把链表接回中央链表并更新各追踪记录。一次归还链表可以包含多个 Span 的 Block。

每个大小类的 `spanTrackers_` 是 `std::vector<SpanTracker>`，不是固定长度追踪数组。每条记录包含 Span 起始地址、页数、总 Block 数和中央空闲 Block 数：

| 字段 | 含义 |
|---|---|
| `blockCount` | 该 Span 按当前大小类切分出的 Block 总数 |
| `freeCount` | 当前位于 CentralCache 空闲链表中的 Block 数 |

`freeCount` 不统计 ThreadCache 本地缓存或用户仍在使用的 Block。中央链表取走 Block 时相应减少；归还时按所属 Span 增加。只有 `freeCount == blockCount`，CentralCache 才能摘除该 Span 的中央链表节点、删除追踪记录并把 Span 交还 PageCache。

查找 Block 所属 Span 时，当前实现线性扫描对应大小类的追踪器。批量归还还会遍历追踪器并扫描归还链表以按 Span 计数，因此这些路径的成本会随追踪器数量和批次长度增长。

## PageCache

[`include/PageCache.h`](include/PageCache.h) 与 [`src/PageCache.cpp`](src/PageCache.cpp) 管理连续逻辑 Page。当前 `PAGE_SIZE` 为 4096 字节，多个连续 Page 组成 Span。

- `freeSpans_` 按 Span 页数建立有序桶，每个桶的值是该页数的空闲 Span 链表。
- `spanMap_` 按 Span 起始地址记录仍然存在的 Span 元数据，用来定位释放对象及相邻 Span。
- `Span::isFree` 标记 Span 是否在空闲链表中。所有操作完成后的稳定状态下，`isFree == true` 当且仅当该 Span 位于 `freeSpans_`。

分配时，PageCache 使用 `lower_bound` 找到页数足够的空闲 Span；若较大，则切分出请求部分和尾部剩余部分，并分别维护地址映射。释放时，它先用 `spanMap_` 确认起始地址，再用 `isFree` 拒绝重复归还；随后检查地址有序映射中的前驱和右侧相邻 Span。邻居只有在确实连续且空闲链表摘除成功时才参与合并。被合并吸收的 Span 元数据和映射会删除，合并后的 Span 再按新页数加入空闲桶。

没有合适的缓存 Span 时，`systemAlloc()` 使用 `mmap` 获取虚拟内存。归还 PageCache 只表示内存重新进入分配器的空闲 Span 管理；代码没有 `munmap()` 路径，因此不等于将映射归还操作系统。`std::malloc` 本身也是运行库接口，不应简单理解成每次都进行系统调用。

## 核心不变式

这些条件描述了操作完成后的所有权与账目关系。并发操作进行中，必须在相应锁保护下观察；不能依赖无同步的瞬时检查。

1. **本地链表与计数一致**：每个大小类的 `freeListSize_` 等于 ThreadCache 本地 Free List 的实际节点数。若链表是 `A → B → nullptr`，计数就是 2。
2. **中央计数范围明确**：`freeCount` 只数中央链表里的 Block。比如 Span 有 100 个块，中央缓存持有 40 个、本地缓存持有 20 个、用户持有 40 个，则 `blockCount == 100`、`freeCount == 40`。
3. **整 Span 才可回收**：只有 `freeCount == blockCount`，才表明该 Span 的所有块都已回到 CentralCache；此时才可把整个 Span 交回 PageCache。
4. **PageCache 状态与容器一致**：稳定状态下，`Span::isFree` 必须与该对象是否在 `freeSpans_` 中一致。切分、摘除、合并和重新入桶时同步更新状态。
5. **退休元数据不能再使用**：Span 被 CentralCache 回收后，其 `SpanTracker` 删除；Span 合并后，被吸收对象的映射和元数据删除。删除后不能继续解引用对应指针。
6. **同一在用块只能有一个所有者**：Block 从 CentralCache 取出后不再留在中央链表；ThreadCache 取出后不再留在本地链表；交给用户后，分配器不能把它再次分配给另一个调用方。

## v2 中的修复与改进

| 模块 | 曾发现的问题 | 当前实现方式 |
|---|---|---|
| ThreadCache | 零字节分配与释放可能选择不同大小类；本地计数可能下溢；空指针或本地重复释放可能破坏链表 | 大小类取整至少使用 `ALIGNMENT`；仅在摘除本地节点时减计数；忽略空指针并检查当前本地链表重复节点 |
| ThreadCache | 线程退出时，本地空闲块可能一直留在 TLS 缓存 | 析构时逐大小类将本地空闲链表归还 CentralCache |
| CentralCache | 单次传输数量、跨 Span 计数和追踪器生命周期需要明确管理 | 最多 32 个节点批量传输；按所属 Span 更新 `freeCount`；整 Span 回收时摘除其中央链表节点并退休追踪记录 |
| PageCache | 空闲桶维护、分割映射、重复归还和只向右合并会影响 Span 正确性 | 维护 `isFree` 与空闲桶关系，登记分割剩余 Span，检查重复归还，并在地址映射中合并左右相邻空闲 Span |
| 大小类 | 固定 8 字节分组不能覆盖一般 `max_align_t` 对齐 | 使用 `alignof(std::max_align_t)` 作为大小类粒度 |

这张表概述当前源码能确认的实现，不代表所有边界条件都已证明无误。

## 构建与测试

从仓库根目录配置并构建 v2：

```bash
cmake -S v2 -B build/v2
cmake --build build/v2 -j
```

主要 CMake 目标如下：

| 目标 | 用途 |
|---|---|
| `unit_test` | 基础分配、写入、多线程、边界和压力测试 |
| `page_cache_empty_free_list_test` | 空闲桶与仍在使用 Span 的重叠检查 |
| `page_cache_span_mapping_test` | 分割后尾部 Span 映射与重新合并 |
| `page_cache_double_free_test` | PageCache 重复归还 |
| `page_cache_bidirectional_coalescing_test` | Span 双向合并 |
| `allocator_safety_test` | CentralCache Span 回收、追踪记录和多 Span 节点管理 |
| `zero_size_batch_return_test` | 零字节请求与批量归还后的地址复用 |
| `thread_cache_exit_test` | TLS 缓存在线程退出时归还及后续复用 |
| `perf_test` / `perf` | 构建性能程序 / 构建并运行性能程序 |
| `test` | 构建并运行基础测试和上述回归测试 |

运行单元测试和完整测试目标：

```bash
cmake --build build/v2 --target unit_test
./build/v2/unit_test
cmake --build build/v2 --target test
```

运行性能程序：

```bash
cmake --build build/v2 --target perf
```

`test` 是项目自定义的 CMake target，不是已注册给 CTest 的测试套件；当前配置没有 `enable_testing()` / `add_test()`。性能程序也没有公布固定基准结果，运行结果应结合目标机器和负载自行分析。

### 可选 Sanitizer 构建

项目没有单独配置 Sanitizer target，可用支持相应选项的 GCC 或 Clang 建立独立构建目录。ASan/UBSan 与 TSan 应分开构建：

```bash
# ASan + UBSan
cmake -S v2 -B build/v2-asan -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build/v2-asan --target test -j

# TSan：使用独立目录，不与 ASan 混用
cmake -S v2 -B build/v2-tsan -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread"
cmake --build build/v2-tsan --target thread_cache_exit_test unit_test -j
./build/v2-tsan/thread_cache_exit_test
./build/v2-tsan/unit_test
```

Sanitizer 命令是可选的运行方式，不表示这些配置已在所有平台验证通过。由于两个共享缓存单例采用进程生命周期分配，LeakSanitizer 可能将其报告为进程结束时仍存活的对象；这与常规越界和生命周期错误需要分别判断。

## 当前限制与后续方向

- **Span 查找成本**：`getSpanTracker()` 在线性扫描同一大小类的追踪器；归还时还要按 Span 扫描本批节点。当前实现优先保持教学可读性，没有为追踪器建立更快的地址索引。
- **中央链表摘除成本**：回收完整 Span 时，需要遍历该大小类中央空闲链表移除属于它的节点。
- **固定大小类与缓存占用**：每个 ThreadCache 为全部大小类保留链表头和计数数组；实际占用按 `FREE_LIST_SIZE`、指针宽度和对齐粒度变化。向上取整也会产生内部碎片。
- **线程间缓存不均**：ThreadCache 的空闲块留在线程本地，直到达到归还阈值或线程退出；不同线程之间的负载不均可能暂时造成内存分布不均。
- **大对象路径**：超过 256 KiB 的请求直接走 `malloc/free`，没有通过 PageCache 统一管理，也没有在本项目中实现大对象缓存策略。
- **操作系统归还**：PageCache 没有 `munmap()` 逻辑，已映射区域会被缓存复用，直到进程结束由操作系统回收。
- **异常与输入校验**：核心路径包含部分失败处理，但并未提供完整的生产级异常安全或任意错误指针校验。PageCache 的重复归还检查依赖有效的 Span 起始地址和内部状态；调用者仍须传入正确的指针、大小并遵守单次释放规则。
- **重复释放检测边界**：ThreadCache 只检测当前本地链表中的重复节点；它不是全局分配状态表，不能可靠识别所有跨线程或已转入中央缓存后的无效释放。
- **测试范围**：现有测试覆盖若干回归路径和基础并发场景，但不等同于穷举验证；`UnitTest.cpp` 的一部分检查使用 `assert`，在定义 `NDEBUG` 时会被移除。
- **性能验证**：自带性能程序使用有限的分配模式做对比，不是严谨的通用基准；未提供可复现的性能结论或提升比例。

## 阅读顺序

1. [`include/MemoryPool.h`](include/MemoryPool.h)：对外入口。
2. [`include/Common.h`](include/Common.h)：大小类及边界常量。
3. [`src/ThreadCache.cpp`](src/ThreadCache.cpp)：本地分配、归还和线程退出处理。
4. [`src/CentralCache.cpp`](src/CentralCache.cpp)：批量链表操作及 SpanTracker 计数。
5. [`src/PageCache.cpp`](src/PageCache.cpp)：Span 分割、状态维护与双向合并。
6. [`tests/`](tests/) 与 [`CMakeLists.txt`](CMakeLists.txt)：回归场景及其构建目标。

