# C++ High Performance Memory Pool

一个轻量级、线程安全且高性能的 C++ 固定大小对象内存池实现。专为游戏开发（如子弹、粒子系统）或高频小对象分配场景设计。

## ✨ 特性 (Features)

*   **⚡ 高性能 (High Performance)**:
    *   **侵入式空闲链表 (Intrusive FreeList)**: 无需额外内存开销来维护空闲块，将空闲对象的内存复用为链表节点。
    *   **内存大页管理 (Page Management)**: 批量申请大块内存（Page），减少 `malloc`/`new` 的系统调用次数，降低堆碎片。
    *   **内存对齐 (Aligned Allocation)**: 自动处理内存对齐（支持 `posix_memalign` 和 Windows `_aligned_malloc`），对 SIMD 优化友好。
*   **🛡️ 线程安全 (Thread Safety)**: 内置 `std::mutex` 互斥保护，支持多线程并发访问。
*   **📈 动态伸缩 (Dynamic Scaling)**:
    *   **自动扩容**: 内存不足时自动申请新的 Page。
    *   **智能收缩 (Adaptive Shrinking)**: 引入类“强化学习”的峰值追踪算法。系统会记录长期使用的峰值水位，当检测到长时间空闲且实际用量远低于水位时，自动触发垃圾回收 (GC) 并释放内存归还给操作系统。
*   **🔒 安全可靠 (Safety)**:
    *   **RAII**: 利用构造/析构函数管理生命周期，禁用拷贝防止双重释放。
    *   **异常安全**: 扩容逻辑使用 `std::unique_ptr` 保护，防止扩容失败时的内存泄漏。
    *   **边界检查**: 提供 `contains()` 方法辅助调试，验证指针合法性。

## 🛠️ 构建 (Build)

本项目支持 Windows (MinGW) 和 Linux 环境。需要支持 C++11 或更高版本的编译器。

### Windows (使用脚本)
直接运行提供的批处理脚本：
```cmd
.\build.bat
```

### 通用 (使用 Make)
```bash
make
```

### 使用 CMake
```bash
mkdir build
cd build
cmake ..
cmake --build .
```

## 🚀 快速开始 (Usage)

只需包含头文件 `include/MemoryPool.hpp` 即可使用。

```cpp
#include "MemoryPool.hpp"

struct Bullet {
    int id;
    double damage;
    // ...
};

int main() {
    // 创建一个管理 Bullet 对象的内存池
    MemoryPool<Bullet> pool;

    // 1. 分配对象 (类似于 new Bullet(1, 50.0))
    // allocate 支持任意构造参数
    Bullet* b1 = pool.allocate(1, 50.0);

    // 2. 使用对象
    std::cout << "Bullet ID: " << b1->id << std::endl;

    // 3. 回收对象 (类似于 delete b1)
    pool.deallocate(b1);
    
    return 0;
}
```

## ⚙️ 核心配置

在 `MemoryPool.hpp` 中可以调整以下常量以适应不同场景：

```cpp
const size_t INITIAL_SIZE = 5000;      // 初始容量
const size_t GROW_SIZE = 2000;         // 每次扩容数量
const size_t MAINTAIN_INTERVAL = 1000; // 触发自动维护（GC）的频率
```

## 📄 License

MIT License
