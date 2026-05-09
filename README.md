# High Performance Memory Pool

一个基于 `C++17` 的高性能内存池模板样例项目，面向高频分配/释放固定大小对象的场景，例如游戏实体、粒子系统、网络会话和短生命周期任务对象。

本项目以 **Docker/Linux** 作为唯一标准构建与测试环境。Windows 本地开发建议通过 **WSL2 + Docker Desktop** 运行 `make` 目标，不再维护原生 Windows 工具链或脚本入口。

---

## 项目结构

```text
Memory_Pool/
├── include/
│   └── MemoryPool.hpp              # 核心实现：内存池模板类、页管理、线程局部缓存
├── src/
│   └── main.cpp                    # 交互式演示程序：多线程仿真、状态查看、性能对比
├── tests/
│   └── memory_pool_smoke_test.cpp  # 自动冒烟测试：基础分配、批量分配、多线程使用
├── Dockerfile                      # Docker/Linux 构建环境
├── .dockerignore                   # Docker 构建上下文忽略规则
├── CMakeLists.txt                  # CMake 构建与 CTest 测试入口
├── Makefile                        # Docker 统一入口
└── README.md                       # 项目说明
```

---

## 核心特性

### 1. 分页管理

内存池以 `Page` 为单位批量申请连续内存，再切分为固定大小对象块，减少频繁系统分配带来的开销和碎片。

### 2. 线程局部缓存

每个线程维护独立的 `ThreadCache`，大多数分配/释放操作只需要操作本地空闲链表。只有本地缓存不足或超过软上限时，才批量访问全局池，从而降低多线程锁竞争。

### 3. 侵入式空闲链表

空闲对象内存被复用为 `FreeNode` 节点，不额外维护外部节点结构，降低元数据开销。

### 4. Linux 对齐分配

`AlignedAllocator` 基于 `posix_memalign` 实现对齐分配，适配 Docker/Linux 标准环境。

### 5. 自动维护与收缩

内存池根据长期峰值和当前容量判断是否触发 `shrink()`，尝试释放完全空闲且非近期活跃的页。

---

## 环境要求

- Docker
- GNU Make
- Windows 用户建议启用 WSL2，并在 WSL 中执行命令

Docker 镜像内包含：

- `build-essential`
- `cmake`
- `make`
- UTF-8 运行环境

---

## 构建、测试与运行

### 构建 Docker 镜像

```bash
make image
```

### 编译项目

```bash
make build
```

该命令会在 Docker 容器中执行：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

### 运行自动测试

```bash
make test
```

该命令会在 Docker 容器中执行 `ctest`，当前测试入口为 `memory_pool_smoke_test`。

### 运行交互式演示程序

```bash
make run
```

程序启动后支持以下命令：

- `<ThreadID> <Count>`：让指定工作线程创建指定数量的模拟对象，例如 `1 10000`。
- `debug`：切换基准测试输出。
- `status`：查看当前内存池统计信息。
- `clear`：强制回收当前管理器持有的活跃对象，并通知工作线程刷新本地缓存。
- `flush`：刷新主线程本地缓存。
- `exit`：退出程序。

### 进入容器 Shell

```bash
make shell
```

### 清理构建产物

```bash
make clean
```

---

## CMake 目标

- `memory_pool`：header-only 接口库，暴露 `include/` 并要求 `cxx_std_17`。
- `memory_pool_demo`：交互式演示程序。
- `memory_pool_smoke_test`：自动冒烟测试程序。

---

## 作为模板项目使用

如果要在其他项目中复用内存池核心实现，推荐只引入：

```text
include/MemoryPool.hpp
```

业务侧可以按需创建自己的对象类型，并使用：

```cpp
MemoryPool<MyObject> pool;
MyObject* object = pool.allocate(args...);
pool.deallocate(object);
```

注意事项：

- `T` 的大小必须至少能容纳一个指针，以便空闲时复用为侵入式链表节点。
- 跨线程场景下建议在线程退出前调用 `flush_thread_cache()`。
- `get_stats()` 主要用于运行期观测，不建议作为严格资源生命周期断言。
- 该模板面向固定大小对象池，不适合直接管理可变长内存块。

---

## 常见问题

### 系统 new 的底层一定是 malloc 吗？

不一定。C++ 表达式 `new T(...)` 的标准语义可以拆成两步：先调用 `operator new(sizeof(T))` 获取原始内存，再在这块内存上构造对象；`delete` 则先析构对象，再调用 `operator delete` 释放原始内存。

默认全局 `operator new` 在常见实现中通常会委托给运行时分配器，例如 Linux/glibc 下常见路径会进入 `malloc`/`free` 背后的 `ptmalloc`，Windows/MSVC 下会进入 CRT/系统堆。但 C++ 标准并不强制它必须直接调用 `malloc`，项目或库也可以重载全局或类专属 `operator new`。

因此，系统 `new` 的性能不能简单理解为“每次都向操作系统申请内存”。现代运行时分配器通常包含线程本地缓存、size class、arena/tcache、批量向中心堆申请等优化；小对象反复分配释放时，大量操作可能只在用户态缓存中完成，不会每次触发系统调用。这也是当前内存池需要尽量压短 fast path、减少哈希查找和原子统计开销的原因。

### 为什么释放对象后统计中的 Used Count 不一定立即归零？

线程局部缓存会暂存一批空闲节点，释放对象后节点可能仍停留在当前线程的 `ThreadCache` 中。需要调用 `flush_thread_cache()`，或通过 `clear` 命令通知工作线程刷新本地缓存后，统计值才会进一步回落。

### 为什么只支持 Docker/Linux？

本项目定位为标准化模板样例，优先保证构建、测试、运行环境一致。Windows 本地环境通过 WSL2 + Docker 兼容，不再维护原生 Windows 编译分支。
