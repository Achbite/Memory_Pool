#pragma once
#include <vector>
#include <mutex>
#include <atomic>
#include <iostream>
#include <thread>
#include <vector>
#include <chrono>
#include <memory>
#include <cstddef>
#include <algorithm>
#include <map>

// 内存对齐分配器
// 封装了不同平台的内存对齐分配函数
class AlignedAllocator {
public:
    // 分配对齐内存
    static void* allocate(size_t size, size_t alignment) {
#ifdef _WIN32
        return _aligned_malloc(size, alignment);
#else
        // POSIX 标准要求 alignment 必须是 2 的幂，且 size 必须是 alignment 的倍数
        // 如果不是倍数，某些实现可能会失败，所以向上取整是个好习惯（虽然这里我们只做 wrapper）
        // 对于 aligned_alloc(alignment, size):
        // C++17 标准：void* aligned_alloc( std::size_t alignment, std::size_t size );
        // 注意：C++11下可能没有std::aligned_alloc，需要用posix_memalign
        
        // 为了兼容性，这里使用 posix_memalign
        void* ptr = nullptr;
        if (posix_memalign(&ptr, alignment, size) != 0) {
            throw std::bad_alloc();
        }
        return ptr;
#endif
    }
    
    // 释放对齐内存
    static void deallocate(void* ptr) {
#ifdef _WIN32
        _aligned_free(ptr);
#else
        free(ptr);
#endif
    }
};

// 内存块定义
// 侵入式节点：这是一种节省内存的技巧。
// 当对象空闲时，这块内存本身并不存储数据，而是被解释为指向下一个空闲块的指针。
// 这样就不需要额外的内存来维护空闲链表。
struct FreeNode {
    FreeNode* next;
};

// 线程局部缓存配置
struct ThreadCache {
    FreeNode* free_list = nullptr;
    size_t count = 0;              // 当前缓存的对象数量
    const size_t BATCH_SIZE = 100; // 批量传输大小
    const size_t MAX_CACHE = 200;  // 缓存上限，超过则归还一半给全局池
};

// 内存大页，存放一批对象
struct Page {
    void* memory;    // 指向原始内存块的指针
    size_t size;     // 单个对象大小
    size_t capacity; // 该页能容纳的对象数量
    std::chrono::steady_clock::time_point last_active; // 用于超时检测，判断该页是否很久没用了
    size_t active_count = 0; // 该页当前存活的对象数量 (仅在GC时统计使用)

    // 构造函数：申请原始内存
    // 修复：初始化列表顺序应与成员声明顺序一致 (size 先于 capacity)
    Page(size_t cap, size_t obj_size) : size(obj_size), capacity(cap) {
        // 使用对齐分配器
        // 对齐值通常取 sizeof(void*) 或 CPU cache line 大小 (e.g., 64 bytes)
        // 这里为了保证 T 类型的对齐要求，取 alignof(max_align_t) 或 至少 sizeof(void*)
        // 如果 T 有更严格的对齐要求，应该使用 alignof(T)
        // 这里简化逻辑，确保至少按指针大小对齐，通常足以满足大部分需求
        
        // 修正：GCC 4.9.2 MinGW 中 max_align_t 可能在全局命名空间
        size_t alignment = alignof(max_align_t); 
        if (alignment < sizeof(void*)) alignment = sizeof(void*);

        memory = AlignedAllocator::allocate(cap * obj_size, alignment);
        if (!memory) throw std::bad_alloc();
        
        last_active = std::chrono::steady_clock::now();
    }

    // 析构函数：释放原始内存
    ~Page() {
        if (memory) AlignedAllocator::deallocate(memory);
    }

    // 辅助函数：判断指针是否在页内
    bool contains(void* ptr) const {
        char* start = static_cast<char*>(memory);
        char* end = start + (capacity * size);
        return ptr >= start && ptr < end;
    }
};

template<typename T>
class MemoryPool {
private:
    // 静态断言：编译期检查。
    // 确保对象的大小至少能存下一个指针（FreeNode），否则无法挂入空闲链表。
    static_assert(sizeof(T) >= sizeof(FreeNode), "Object too small for intrusive list");

    std::mutex mtx_;
    std::vector<Page*> pages_;      // 持有所有申请的大块内存页
    FreeNode* free_list_ = nullptr; // 空闲链表头指针，指向当前可用的内存块

    // 统计信息
    std::atomic<size_t> total_capacity_{0};
    std::atomic<size_t> used_count_{0};
    std::atomic<size_t> alloc_count_{0};
    std::atomic<size_t> free_count_{0};

    // 线程局部缓存
    static thread_local ThreadCache t_cache_;

    const size_t INITIAL_SIZE = 5000;
    const size_t GROW_SIZE = 2000; // 每次动态扩容的数量
    
    // 动态伸缩配置
    size_t min_capacity_ = 5000;
    size_t max_capacity_ = 1000000;
    size_t long_term_peak_ = 0; // 长期观察到的峰值使用量
    size_t maintain_ops_counter_ = 0; // 计数器，用于触发维护
    const size_t MAINTAIN_INTERVAL = 1000; // 每1000次deallocate检查一次

public:
    MemoryPool() {
        expand(INITIAL_SIZE);
    }

    // 析构函数：释放所有页
    ~MemoryPool() {
        for (auto page : pages_) delete page;
        pages_.clear();
    }

    // --- 禁止拷贝和移动语义 ---
    // 内存池管理着复杂的内存资源，拷贝会导致两个池管理同一块内存，
    // 析构时会导致双重释放（Double Free）。
    // 因此必须显式禁用拷贝构造和拷贝赋值。
    MemoryPool(const MemoryPool&) = delete;
    MemoryPool& operator=(const MemoryPool&) = delete;
    
    // 移动语义在目前的设计中较为复杂（涉及锁和状态转移），
    // 且通常内存池作为全局单例使用，暂不实现移动语义。
    MemoryPool(MemoryPool&&) = delete;
    MemoryPool& operator=(MemoryPool&&) = delete;
    // ---------------------------

    // 扩容逻辑：申请新的 Page 并将其切分为小块挂入 free_list
    void expand(size_t object_count) {
        // 硬限制：防止无限膨胀
        if (total_capacity_ >= max_capacity_) {
            // 这里可以选择抛出异常或返回空，或者强制回收
            // 为保证稳定性，这里仅打印警告，仍允许分配（除非物理内存耗尽）
            std::cerr << "[Warning] Pool reached max capacity hint.\n";
        }

        // 使用 unique_ptr 临时接管新 Page 的生命周期 (RAII)。
        std::unique_ptr<Page> new_page_ptr(new Page(object_count, sizeof(T)));
        
        // 保持 pages_ 有序，以便使用二分查找
        // 注意：新分配的堆内存地址通常是递增的，但不绝对。为了严谨，我们需要插入到正确位置。
        // 但 vector 插入开销 O(N)。Page 数量不多，且 expand 频率低，可以接受。
        Page* raw_ptr = new_page_ptr.get();
        
        auto it = std::upper_bound(pages_.begin(), pages_.end(), raw_ptr, 
            [](const Page* a, const Page* b) {
                return a->memory < b->memory;
            });
        
        pages_.insert(it, raw_ptr);
        new_page_ptr.release(); // 释放 unique_ptr 所有权
        
        total_capacity_ += object_count;

        // 将新申请的内存切分成块，挂入 free_list
        // static_cast 转换 void* 为 char* 是为了能进行字节级的指针运算
        char* ptr = static_cast<char*>(raw_ptr->memory);
        
        // 遍历这块大内存，将其均分为 count 个块
        // 这里的逻辑相当于在未初始化的内存上建立了一个链表
        for (size_t i = 0; i < object_count; ++i) {
            // 计算第 i 个块的地址
            // reinterpret_cast 是强制类型转换，把这块内存强行解释为 FreeNode
            FreeNode* node = reinterpret_cast<FreeNode*>(ptr + i * sizeof(T));
            
            // 头插法插入空闲链表
            node->next = free_list_;
            free_list_ = node;
        }
    }

    // 从全局池批量获取节点到线程缓存
    // 返回获取到的数量
    size_t fetch_from_global(size_t count) {
        std::lock_guard<std::mutex> lock(mtx_);
        
        // 如果全局池空了，先扩容
        if (!free_list_) {
            expand(std::max(count, GROW_SIZE));
            // std::cout << "[System] Pool Expanded for Thread Cache. Capacity: " << total_capacity_ << "\n";
        }

        size_t fetched = 0;
        // 从全局 free_list 搬运节点到线程缓存
        while (free_list_ && fetched < count) {
            FreeNode* node = free_list_;
            free_list_ = node->next; // 摘除
            
            // 挂入线程缓存
            node->next = t_cache_.free_list;
            t_cache_.free_list = node;
            
            fetched++;
            used_count_++; // 从全局池的角度看，分配给线程缓存就算 "used"
        }
        return fetched;
    }

    // 将线程缓存归还一部分给全局池
    void return_to_global(size_t count) {
        if (count == 0 || !t_cache_.free_list) return;

        std::lock_guard<std::mutex> lock(mtx_);
        
        size_t returned = 0;
        // 循环：将链表节点逐一摘下，还给全局 free_list
        // 这是连接线程私有领域和全局共享领域的关键步骤
        while (t_cache_.free_list && returned < count) {
            FreeNode* node = t_cache_.free_list;
            t_cache_.free_list = node->next; // 摘除
            
            // 挂回全局池
            node->next = free_list_;
            free_list_ = node;
            
            returned++;
            used_count_--; // 从全局池回收
        }

        // 触发维护逻辑：当有内存归还时，检查是否需要 GC 收缩
        // 增加计数器，避免每次归还都触发昂贵的 maintain 操作（涉及浮点运算和可能的锁竞争分析）
        // MAINTAIN_INTERVAL = 1000，这里取其 1/10，即每归还 ~10 个 batch 后检查一次
        maintain_ops_counter_++;
        if (maintain_ops_counter_ >= MAINTAIN_INTERVAL / 10) { 
            maintain_ops_counter_ = 0;
            maintain();
        }
    }

    // 强制刷新线程缓存，将所有节点归还给全局池
    // 建议在线程任务结束、帧结束或空闲时调用，以降低看似泄漏的内存占用
    void flush_thread_cache() {
        if (!t_cache_.free_list) return;
        
        // 全额归还
        return_to_global(t_cache_.count);
        t_cache_.count = 0;
        t_cache_.free_list = nullptr;
    }

    // 类强化学习的动态维护：根据历史峰值调整容量
    void maintain() {
        // 1. 更新观测数据
        size_t current_usage = used_count_;
        if (current_usage > long_term_peak_) {
            // 发现新峰值，迅速跟进
            long_term_peak_ = current_usage;
        } else {
            // 峰值衰减：模拟遗忘机制
            // 如果确实很长时间用不到那么多，慢慢降低预期
            // 衰减因子 0.999 表示比较保守，不愿意轻易释放内存
            long_term_peak_ = static_cast<size_t>(long_term_peak_ * 0.999);
        }
        
        // 保证下限
        if (long_term_peak_ < min_capacity_) long_term_peak_ = min_capacity_;

        // 2. 计算目标容量
        // 目标容量定为 1.2 倍的历史峰值，留有余量
        size_t target_capacity = long_term_peak_ * 1.2;
        if (target_capacity < min_capacity_) target_capacity = min_capacity_;

        // 3. 判断是否需要收缩 (Shrink)
        // 只有当当前容量显著大于目标容量（例如 1.5 倍）时才触发，避免频繁抖动 (Hysteresis)
        // 施密特触发器原理：上升阈值高，下降阈值低，防止在临界点反复震荡
        if (total_capacity_ > target_capacity * 1.5) {
            shrink(target_capacity);
        }
    }

    // 内存收缩（GC）：回收完全空闲的页
    // 这是一个相对耗时的操作，不需要太频繁调用
    void shrink(size_t target_capacity) {
        // 必须持有锁，因为要操作 free_list_ 和 pages_
        // 由于 deallocate 持有锁，我们将其作为私有函数直接执行。

        // 1. 统计每个 Page 的空闲节点数
        // 这一步需要遍历整个 free_list_，O(FreeNodes)
        // Page 数量较少，可以用 map 或者重置 active_count
        
        for (auto page : pages_) {
            page->active_count = page->capacity; // 初始假设全是满的
        }

        FreeNode* curr = free_list_;
        while (curr) {
            // 查找 node 属于哪个 Page
            // 利用 pages_ 已排序的特性进行二分查找
            auto it = std::upper_bound(pages_.begin(), pages_.end(), curr, 
                [](const void* addr, const Page* page) {
                    return addr < page->memory;
                });
            
            // upper_bound 返回第一个 memory > addr 的页，所以目标页是 it - 1
            if (it != pages_.begin()) {
                Page* page = *(--it);
                if (page->contains(curr)) {
                    page->active_count--; // 找到一个空闲节点，存活数 -1
                }
            }
            curr = curr->next;
        }

        // 2. 识别可回收的页
        std::vector<Page*> pages_to_keep;
        std::vector<Page*> pages_to_free;
        size_t kept_capacity = 0;

        for (auto page : pages_) {
            // 如果页完全空闲 (active_count == 0) 且 我们还需要削减容量
            if (page->active_count == 0 && (total_capacity_ - page->capacity) >= target_capacity) {
                pages_to_free.push_back(page);
                total_capacity_ -= page->capacity;
            } else {
                pages_to_keep.push_back(page);
                kept_capacity += page->capacity;
            }
        }

        if (pages_to_free.empty()) return;

        // 3. 重建 FreeList
        // 只保留属于 pages_to_keep 的节点
        // 这是一个 O(FreeNodes) 的操作
        FreeNode* new_free_list = nullptr;
        // 尾指针优化，避免每次都遍历寻找尾部
        FreeNode** tail_ptr = &new_free_list; 

        curr = free_list_;
        while (curr) {
            FreeNode* next_node = curr->next; // 先保存下一个，因为 curr 可能被跳过

            // 让我们用更笨但稳健的方法：
            // 判断 curr 是否落在 pages_to_free 的任何一个区间内。
            bool is_garbage = false;
            // 遍历待释放页 (通常数量很少，性能可接受)
            for (auto p : pages_to_free) {
                if (p->contains(curr)) {
                    is_garbage = true;
                    break;
                }
            }

            if (!is_garbage) {
                // 保留：挂到新链表末尾
                *tail_ptr = curr;
                tail_ptr = &curr->next;
            }

            curr = next_node;
        }
        *tail_ptr = nullptr; // 链表收尾
        free_list_ = new_free_list;

        // 4. 物理删除页
        for (auto p : pages_to_free) {
            delete p;
        }
        
        // 5. 更新页列表
        pages_ = pages_to_keep;
        
        // std::cout << "[System] Shrink triggered. Freed " << pages_to_free.size() << " pages.\n";
    }

    // 分配对象
    // Args&&... args: 变长模板参数，支持任意形式的构造函数参数
    template<typename... Args>
    T* allocate(Args&&... args) {
        // 1. 尝试从线程局部缓存分配 (无锁路径)
        if (!t_cache_.free_list) {
            // 缓存为空，批量从全局池获取
            fetch_from_global(t_cache_.BATCH_SIZE);
            // 如果获取后依然为空（理论上 fetch_from_global 内部会 expand，不会空），则真的无法分配（OOM）
            if (!t_cache_.free_list) throw std::bad_alloc();
        }

        // 从线程局部缓存摘取节点
        FreeNode* node = t_cache_.free_list;
        t_cache_.free_list = node->next;
        t_cache_.count--;
        
        alloc_count_++; // 统计总分配数 (atomic)

        // --- Placement New ---
        // 这里的语法是 new (address) Type(args...)
        // 它的作用是：不分配新内存，而是直接在 node 指向的这块现有内存上,
        // 调用 T 的构造函数。
        // 这是内存池的核心：复用内存，避免反复向操作系统申请。
        return new (node) T(std::forward<Args>(args)...);
    }

    // 回收对象
    void deallocate(T* ptr) {
        // 基础边界检查：禁止回收空指针
        // 注意：这里没有检查 ptr 是否真的属于本内存池（为了性能）。
        // 如果传入了一个野指针或者栈上变量的指针，这里会导致未定义行为。
        if (!ptr) return;

        // 1. 显式调用析构函数
        // 因为我们用的是 placement new，编译器不会自动管理析构，
        // 必须手动调用 ~T() 来清理对象内部可能申请的资源。
        ptr->~T();

        // 2. 归还到线程局部缓存 (无锁路径)
        FreeNode* node = reinterpret_cast<FreeNode*>(ptr);
        node->next = t_cache_.free_list;
        t_cache_.free_list = node;
        t_cache_.count++;
        
        free_count_++; // 统计总释放数 (atomic)

        // 3. 检查缓存是否过爆，需要批量归还全局池
        if (t_cache_.count >= t_cache_.MAX_CACHE) {
            // 归还一半
            return_to_global(t_cache_.BATCH_SIZE); 
            t_cache_.count -= t_cache_.BATCH_SIZE;
        }

        // 维护逻辑：这里稍微有点复杂，因为现在维护是由主线程或者 deallocate 触发的
        // 以前是每次 deallocate 全局锁去触发。
        // 现在大部分时间不碰全局锁。
        // 为了保持 maintain 功能，我们可以在 return_to_global 里做。
        // 或者简单点：TLAB 模式下，maintain 的频率本身就会降低（只有批量归还时才可能触发），
        // 我们可以把 maintain 放到 return_to_global 里调用。
    }

    // 边界检查：判断指针是否属于该内存池
    // 这是一个 O(N) 操作，N 为 Page 页数。
    // 主要用于调试或在不确定指针来源时进行验证。
    bool contains(T* ptr) {
        std::lock_guard<std::mutex> lock(mtx_);
        for (auto page : pages_) {
            // 计算当前页的地址范围
            // start: 页内存起始地址
            // end: 页内存结束地址
            char* start = static_cast<char*>(page->memory);
            char* end = start + (page->capacity * page->size);
            
            // 检查 ptr 是否在 [start, end) 范围内
            if (reinterpret_cast<char*>(ptr) >= start && reinterpret_cast<char*>(ptr) < end) {
                return true;
            }
        }
        return false;
    }

    // 获取统计信息字符串
    void get_stats(size_t& out_alloc, size_t& out_free, size_t& out_used, size_t& out_cap) {
        out_alloc = alloc_count_.load();
        out_free = free_count_.load();
        out_used = used_count_.load();
        out_cap = total_capacity_.load();
    }
    
    // 重置瞬时统计（生成/销毁数在每轮打印后重置）
    void reset_round_stats() {
        alloc_count_ = 0;
        free_count_ = 0;
    }
};

// 静态成员定义
template<typename T>
thread_local ThreadCache MemoryPool<T>::t_cache_;
