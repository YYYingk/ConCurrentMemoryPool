# ConcurrentMemoryPool

> A high-performance concurrent memory allocator inspired by **Google TCMalloc**, featuring a three-tier caching architecture designed for multi-threaded scenarios.

[中文](#项目简介) | [Architecture](#architecture)

---

## 项目简介

本项目是一个基于 **C++11** 实现的高并发内存池，核心设计参考了 Google 的 **TCMalloc (Thread-Caching Malloc)**。通过 **ThreadCache → CentralCache → PageCache** 的三级缓存架构，配合 **TLS (Thread Local Storage)** 无锁分配、细粒度桶锁、页级合并回收等机制，在多线程高并发场景下显著降低内存分配的竞争开销。

---

## ✨ 核心特性

- **三层缓存架构**：ThreadCache（线程私有）→ CentralCache（共享中心缓存）→ PageCache（页级管理）
- **TLS 无锁分配**：线程优先从本地 `thread_local` 缓存分配，无需加锁
- **细粒度并发控制**：CentralCache 采用**桶级锁**（每个大小类一把锁），而非全局大锁
- **页合并回收**：PageCache 支持释放 Span 时的**前后相邻页合并**，有效缓解内存碎片
- **O(1) 页映射**：采用 **三层基数树 (Radix Tree)** 实现页号到 Span 的常数级查找
- **对象池复用**：`ObjectPool` 预分配内存块管理 `Span` / `ThreadCache` 对象，减少系统调用
- **对齐策略**：针对不同大小范围采用 8B / 16B / 128B / 1KB / 8KB 多级对齐，平衡内存碎片与桶数量

---

## 🏗️ 架构设计

```
┌─────────────────────────────────────────────────────────────────┐
│                        User Code                                │
│           ConcurrentAlloc(size) / ConcurrentFree(ptr)           │
└────────────────────────────┬────────────────────────────────────┘
                             │
                             ▼
┌─────────────────────────────────────────────────────────────────┐
│                      ThreadCache (TLS)                          │
│  ┌─────────────────────────────────────────────────────────┐   │
│  │  thread_local ThreadCache* pTLSThreadCache              │   │
│  │  ├─ FreeList[208]  (per-size freelists)                │   │
│  │  │   ├─ Allocate(): Pop from local freelist (lock-free)│   │
│  │  │   └─ Deallocate(): Push to local freelist           │   │
│  │  └─ FetchFromCentralCache(): batch fetch when empty    │   │
│  └─────────────────────────────────────────────────────────┘   │
└────────────────────────────┬────────────────────────────────────┘
                             │  (batch allocation)
                             ▼
┌─────────────────────────────────────────────────────────────────┐
│                      CentralCache (Singleton)                   │
│  ├─ SpanList[208]  (one lock per bucket)                       │
│  │   └─ FetchRangeObj(): move objects from Span to TC         │
│  └─ GetOneSpan(): split / fetch Span from PageCache            │
└────────────────────────────┬────────────────────────────────────┘
                             │  (page-level allocation)
                             ▼
┌─────────────────────────────────────────────────────────────────┐
│                      PageCache (Singleton)                      │
│  ├─ SpanList[129]  (1 ~ 128 pages, 8KB per page)               │
│  │   ├─ NewSpan(k): alloc k pages, support split & merge      │
│  │   └─ ReleaseSpanToPageCache(): merge with prev/next Span   │
│  └─ TCMalloc_PageMap3<64-13>: O(1) page-id → Span lookup       │
└─────────────────────────────────────────────────────────────────┘
```

---

## 🔧 核心机制详解

### 1. ThreadCache — 线程私有缓存

每个线程通过 `thread_local ThreadCache* pTLSThreadCache` 拥有独立的内存缓存：

- **Allocate**：优先从本地 `FreeList[index]` 弹出一个对象。**无锁、O(1)**。
- **Deallocate**：归还到本地 `FreeList[index]`。当链表长度超过 `MaxSize` 时，触发 `ListTooLong()`，将一批对象批量归还 CentralCache。
- **动态扩容**：`MaxSize` 初始为 1，每次向 CentralCache 申请失败则缓慢增长，控制批量申请的幅度。

### 2. CentralCache — 中心缓存（桶级锁）

CentralCache 是全局单例，管理按大小类组织的 `SpanList`：

- **FetchRangeObj**：从对应大小的 Span 中切出 `batchNum` 个对象返回给 ThreadCache。该操作只锁住**对应桶** (`_spanLists[index]._mtx`)，不同大小类之间可并发。
- **GetOneSpan**：如果当前桶没有空闲 Span，向 PageCache 申请一个新的 Span，并将其拆分为若干对象挂到 `Span._freelist` 上。
- **ReleaseListToSpans**：接收 ThreadCache 归还的一批对象，通过**页映射**找到每个对象所属的 Span，挂回 Span 的自由链表。当 `use_count == 0` 时，整个 Span 归还 PageCache。

### 3. PageCache — 页级管理与大块合并

PageCache 管理以页（8KB）为单位的内存块：

- **NewSpan(k)**：
  1. 优先从 `_spanLists[k]` 直接取；
  2. 若没有，从更大的 `_spanLists[i]` (i > k) 拆分出一个 k 页的 Span，剩余部分挂回原桶；
  3. 若都没有，向 OS 申请 128 页（1MB）大内存，递归拆分。
- **ReleaseSpanToPageCache(span)**：
  - 若页数 > 128，直接归还 OS；
  - 否则尝试与**物理地址相邻**的前后 Span 合并，更新 `_idSpanMap`，减少内存碎片。

### 4. 三层基数树页映射 (TCMalloc_PageMap3)

释放内存时，需要通过对象地址找到它属于哪个 Span，从而知道对象大小和归还位置。

本项目采用 **TCMalloc 原版的三层 Radix Tree**：

- 将 64 位地址空间按页号索引；
- `get(PageID)` / `set(PageID, Span*)` 均为 **O(1)**；
- 按需分配节点，而非一次性申请整个数组，节省内存。

相比 `unordered_map`，基数树在查找性能、内存局部性和无锁潜力上都有优势。

### 5. 对象池 (ObjectPool)

`Span`、`ThreadCache` 等对象本身也需要频繁创建/销毁。本项目实现了定长对象池：

- 预先向系统申请 128KB 内存块；
- 内部维护 `FreeList` 回收空闲槽位；
- 对象大小小于指针大小时，按指针大小对齐，保证 `next` 指针可写入。

### 6. 对齐与哈希映射 (SizeClass)

| 申请大小范围  | 对齐粒度 | 桶索引计算                   |
| :------------ | :------- | :--------------------------- |
| [1, 128]      | 8B       | `_Index(size, 3)`            |
| [129, 1024]   | 16B      | `_Index(size-128, 4) + 16`   |
| [1025, 8K]    | 128B     | `_Index(size-1024, 7) + 72`  |
| [8K+1, 64K]   | 1KB      | `_Index(size-8K, 10) + 128`  |
| [64K+1, 256K] | 8KB      | `_Index(size-64K, 13) + 184` |

- 采用位运算 `_RoundUp(size, align) = (size + align - 1) & ~(align - 1)` 实现快速对齐。
- 共 **208** 个自由链表桶，覆盖 8B ~ 256KB 的内存分配需求。
- 大于 256KB 的分配直接走 PageCache，按页对齐。

---

## 📁 目录结构

```
ConcurrentMemoryPool/
├── Common.h              # 公共定义：FreeList, Span, SpanList, SizeClass, 常量
├── ThreadCache.h/.cpp    # 线程本地缓存：Allocate / Deallocate / FetchFromCentralCache
├── CentralCache.h/.cpp   # 中心缓存：FetchRangeObj / GetOneSpan / ReleaseListToSpans
├── PageCache.h/.cpp      # 页缓存：NewSpan / MapObjectToSpan / ReleaseSpanToPageCache
├── PageMap.h             # 三层基数树：TCMalloc_PageMap3<BITS>
├── ObjectPool.h          # 定长对象池：管理 Span / ThreadCache 对象
├── ConcurrentAlloc.h     # 对外接口：ConcurrentAlloc() / ConcurrentFree()
├── benchmark.cpp         # 性能测试：对比 malloc vs ConcurrentAlloc（多线程）
├── test.cpp / Unittest.cpp # 单元测试与功能验证
```

---

## 🚀 编译与运行

### 环境要求

- **Windows** (当前使用 `VirtualAlloc` / `VirtualFree`)
- **C++11** 或更高版本
- Visual Studio / MSVC / MinGW / Clang-cl

> 💡 **Linux 支持**：`SystemAlloc` / `SystemFree` 中已预留 Linux 分支（可使用 `mmap` / `munmap` 或 `sbrk`），只需替换对应系统调用即可跨平台。

### 编译示例 (Visual Studio)

```bash
# 使用 VS 开发者命令行
cl /EHsc /O2 /std:c++11 benchmark.cpp ThreadCache.cpp CentralCache.cpp PageCache.cpp

# 运行
benchmark.exe
```

### 编译示例 (MinGW / MSYS2)

```bash
g++ -std=c++11 -O2 -o benchmark \
    benchmark.cpp ThreadCache.cpp CentralCache.cpp PageCache.cpp \
    -lpthread

./benchmark
```

---

## 📊 Benchmark

测试场景：**4 线程**，每线程 **10,000 次** 分配/释放，对象大小 **16 字节**。

```cpp
BenchmarkConcurrentMalloc(10000, 4, 1);  // 本内存池
BenchmarkMalloc(10000, 4, 1);             // 系统 malloc/free
```

### 典型输出对比

| 指标     | `malloc/free` | `ConcurrentAlloc` | 提升     |
| :------- | :------------ | :---------------- | :------- |
| 分配耗时 | ~xxx ms       | ~xxx ms           | **N 倍** |
| 释放耗时 | ~xxx ms       | ~xxx ms           | **N 倍** |
| 总耗时   | ~xxx ms       | ~xxx ms           | **N 倍** |

> 实际性能数据与具体硬件、并发度、分配大小分布有关。在**固定小对象 + 高并发**场景下，本内存池因避免了系统调用和锁竞争，通常有数倍到数十倍的性能提升。

---

## 💡 技术亮点总结

| 技术点         | 实现方式                          | 收益                        |
| :------------- | :-------------------------------- | :-------------------------- |
| **TLS 无锁**   | `thread_local ThreadCache*`       | 线程本地分配零竞争          |
| **桶级锁**     | `SpanList._mtx` 每桶一把锁        | 不同大小类并发不互斥        |
| **批量申请**   | `FetchRangeObj` 一次取多个对象    | 均摊系统调用和锁开销        |
| **批量归还**   | `ListTooLong` 达到一定长度再还 CC | 减少跨层交互频率            |
| **页合并**     | `ReleaseSpanToPageCache` 前后合并 | 缓解外部碎片                |
| **基数树映射** | `TCMalloc_PageMap3`               | 页号查 Span 为 O(1)         |
| **对象池复用** | `ObjectPool` 预分配 128KB 块      | 减少 Span 本身的 new/delete |

---

## 📝 TODO / 待优化

- [ ] **Linux 平台支持**：将 `SystemAlloc` / `SystemFree` 替换为 `mmap` / `munmap`
- [ ] **跨平台线程 ID**：当前 Windows 依赖部分平台特性，可进一步抽象
- [ ] **大对象优化**：> 256KB 的分配目前直接走 PageCache，可考虑引入专门的定长大块缓存
- [ ] **统计与监控**：增加内存使用量、命中率、碎片率等运行时统计接口
- [ ] **无锁 PageMap**：当前 `_idSpanMap` 读操作在 PageCache 大锁保护下，后续可探索读写锁或无锁方案

---

## 📚 参考资料

- [TCMalloc : Thread-Caching Malloc](https://goog-perftools.sourceforge.net/doc/tcmalloc.html) — Google 官方文档
- [Gperftools 源码](https://github.com/gperftools/gperftools)

---

## 📄 License

本项目为学习目的编写，代码开源，欢迎参考、学习和改进。

如有问题或建议，欢迎提交 Issue 或 PR。
