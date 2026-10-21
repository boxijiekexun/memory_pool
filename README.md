# Kama Memory Pool

这是一个用于学习 C++ 内存管理、空闲链表、缓存分层和并发同步的项目，包含多个实现版本。它适合阅读、实验和比较不同设计，不是生产级通用分配器，也不保证在所有工作负载下都比系统分配器更快。

## 项目结构

```text
v1/  基础固定大小内存池：HashBucket、MemoryPool 与 Slot Free List
v2/  ThreadCache、CentralCache、PageCache 三级缓存实现
v3/  另一份三级缓存实现快照，采用不同的批量获取和 Span 管理方式
```

各版本都有自己的 CMake 配置和测试程序。版本之间是学习过程中的不同实现，不代表后一个版本在所有方面都更完善。

### 版本简介

- **v1**：以 `HashBucket` 将请求分派到固定大小的 `MemoryPool`，每个池管理 Block 和 Slot Free List；使用 CAS 处理空闲链表，并以互斥锁保护新 Block 的分配。
- **v2**：采用 ThreadCache、CentralCache、PageCache 三级结构，重点学习线程本地缓存、批量 Block 转移、Span 追踪、分割与合并。设计和回归测试说明见 [v2/README.md](v2/README.md)。
- **v3**：保留三级缓存的总体思路，但代码是另一种实现：例如大小类使用固定 8 字节粒度，CentralCache 根据请求数量批量取块，Span 跟踪与 v2 不同。请以 [v3 源码](v3/) 为准，不要把 v2 的修复状态套用到 v3。

## v2 架构概览

```text
Application
    ↓
ThreadCache（线程本地 Free List）
    ↓ 批量获取 / 归还
CentralCache（按大小类共享 Free List）
    ↓ Span 分配 / 回收
PageCache（管理连续 Page 组成的 Span）
    ↓ 新内存
mmap
```

常规小块分配先从当前线程的 ThreadCache 获取；本地缺块时向 CentralCache 批量获取；中央缓存需要新空间时再向 PageCache 请求 Span。超过 v2 的 `MAX_BYTES`（256 KiB）的请求走 `malloc/free` 路径。`malloc` 由运行库管理，不意味着每次调用都会触发一次系统调用。

## 在 Linux / WSL 中构建 v2

从仓库根目录运行。需要 CMake、支持 C++17 的编译器和 pthread。

```bash
cmake -S v2 -B build/v2
cmake --build build/v2 -j
```

运行基础单元测试：

```bash
cmake --build build/v2 --target unit_test
./build/v2/unit_test
```

运行 CMake 中配置的完整测试目标（基础测试和 v2 回归测试）：

```bash
cmake --build build/v2 --target test
```

构建并运行性能测试程序：

```bash
cmake --build build/v2 --target perf
```

性能测试只是项目自带的简单对比程序；结果受编译器、运行环境和测试负载影响，不应视为普遍性能结论。


