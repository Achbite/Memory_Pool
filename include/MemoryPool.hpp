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
#include <cstring>

// 可选预热内存页，减少首次访问的缺页中断
// #define MEMORY_POOL_PREHEAT

// 跨平台内存对齐分配器
// 封装 _aligned_malloc (Windows) 和 posix_memalign (Linux/Unix)
// 作用：提供统一的跨平台内存对齐分配接口。
class AlignedAllocator {
public:
    // 分配对齐内存
    // size: 需要分配的内存大小
    // alignment: 对齐字节数
    static void* allocate(size_t size, size_t alignment) {
#ifdef _WIN32
        return _aligned_malloc(size, alignment);
#else
        void* ptr = nullptr;
        if (posix_memalign(&ptr, alignment, size) != 0) {
            throw std::bad_alloc();
        }
        return ptr;
#endif
    }
    
    // 释放对齐内存
    // ptr: 内存指针
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
    FreeNode* next; // 指向下一个空闲节点
};

// 线程局部缓存 (Thread-Local Allocation Buffer - TLAB)
// 核心优化组件：通过 TLS 减少多线程对全局锁的竞争，实现无锁的快速分配与释放。
struct ThreadCache {
    FreeNode* free_list = nullptr; // 本地空闲链表头
    size_t count = 0;              // 当前缓存对象数
    
    // ----------- 调优参数 -----------
    
    // 批量传输大小：当缓存不足或溢出时，与全局池交互的对象数量。
    // 较大的 BATCH_SIZE 可分摊锁竞争 (Lock Contention) 开销。
    const size_t BATCH_SIZE = 512; 

    // 缓存软上限：当本地缓存超过此阈值，触发归还判定逻辑。
    const size_t MAX_CACHE = 4096;

    // ----------- 迟滞策略 (Hysteresis Strategy) -----------
    // 引入延迟归还机制，避免 count 在 MAX_CACHE 附近波动时引发频繁的锁操作 (Thrashing)。
    // 只有当 pending_return_count 累积到 RETURN_THRESHOLD 时，才真正执行系统级归还。
    
    size_t pending_return_count = 0;       // 累积的待归还计数
    const size_t RETURN_THRESHOLD = 1024;  // 触发归还的阈值
};

// 内存页：管理一块连续的堆内存
// 作用：MemoryPool 的基础存储单元，每次扩容分配一个 Page
struct Page {
    void* memory;    // 内存块首地址
    size_t size;     // 单个对象大小 (含对齐)
    size_t capacity; // 容量（对象个数）
    std::chrono::steady_clock::time_point last_active; // 最后活跃时间，用于GC策略
    size_t active_count = 0; // GC 标记阶段使用，记录当前页中被使用的对象数

    // 构造函数：分配大块内存
    // cap: 容量
    // obj_size: 对象大小
    Page(size_t cap, size_t obj_size) : size(obj_size), capacity(cap) {
        // 计算对齐：至少为 void* 大小，或者是 max_align_t
        size_t alignment = alignof(std::max_align_t); 
        if (alignment < sizeof(void*)) alignment = sizeof(void*);

        memory = AlignedAllocator::allocate(cap * obj_size, alignment);
        
        if (memory) {
#ifdef MEMORY_POOL_PREHEAT
            // 预热内存页，减少缺页中断 (Page Fault)
            std::memset(memory, 0, cap * obj_size); 
#endif
        } else {
            throw std::bad_alloc();
        }
        
        last_active = std::chrono::steady_clock::now();
    }

    ~Page() {
        if (memory) AlignedAllocator::deallocate(memory);
    }

    // 检查指针是否属于当前页的内存范围
    bool contains(void* ptr) const {
        char* start = static_cast<char*>(memory);
        char* end = start + (capacity * size);
        return ptr >= start && ptr < end;
    }
};

template<typename T>
class MemoryPool {
private:
    // 确保对象大小至少能容纳一个指针，用于构建空闲链表
    static_assert(sizeof(T) >= sizeof(FreeNode), "Object too small for intrusive list");

    std::mutex mtx_;                // 全局锁，保护 pages_ 和 free_list_
    std::vector<Page*> pages_;      // 持有所有申请的大块内存页
    FreeNode* free_list_ = nullptr; // 全局空闲链表头指针，指向当前可用的内存块

    // 原子计数器，用于统计和监控
    std::atomic<size_t> total_capacity_{0}; // 总容量
    std::atomic<size_t> used_count_{0};     // 当前使用的对象数
    std::atomic<size_t> alloc_count_{0};    // 累计分配次数
    std::atomic<size_t> free_count_{0};     // 累计释放次数
    std::atomic<size_t> cache_hits_{0};      // 缓存命中次数
    std::atomic<size_t> cache_misses_{0};    // 缓存未命中（需访问全局池）次数

    // 线程局部存储 (TLS) 的缓存
    static thread_local ThreadCache t_cache_;

    const size_t INITIAL_SIZE = 5120; // 初始分配大小
    const size_t GROW_SIZE = 5120;    // 每次动态扩容的数量
    
    // 维护相关的参数
    size_t min_capacity_ = 5000;
    size_t max_capacity_ = 1000000;
    size_t long_term_peak_ = 0; // 长期观察到的峰值使用量
    size_t maintain_ops_counter_ = 0; // 计数器，用于触发维护
    const size_t MAINTAIN_INTERVAL = 1000; // 每1000次deallocate检查一次

public:
    MemoryPool() {
        expand(INITIAL_SIZE);
    }

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
    MemoryPool(MemoryPool&&) = delete;
    MemoryPool& operator=(MemoryPool&&) = delete;

    // 扩容：申请新 Page 并将节点挂入全局 Free List
    void expand(size_t object_count) {
        // 硬限制：防止无限膨胀
        if (total_capacity_ >= max_capacity_) {
            // 这里可以选择抛出异常或返回空，或者强制回收
            // 为保证稳定性，这里仅打印警告，仍允许分配（除非物理内存耗尽）
            std::cerr << "[Warning] Pool reached max capacity hint.\n";
        }

        std::unique_ptr<Page> new_page_ptr(new Page(object_count, sizeof(T)));
        Page* raw_ptr = new_page_ptr.get();
        
        // 保持 Page 列表有序，便于二分查找
        auto it = std::upper_bound(pages_.begin(), pages_.end(), raw_ptr, 
            [](const Page* a, const Page* b) {
                return a->memory < b->memory;
            });
        
        pages_.insert(it, raw_ptr);
        new_page_ptr.release();
        
        total_capacity_ += object_count;

        // 初始化新内存块链表
        char* ptr = static_cast<char*>(raw_ptr->memory);
        
        // 遍历这块大内存，将其均分为 count 个块
        // 这里的逻辑相当于在未初始化的内存上建立了一个链表
        for (size_t i = 0; i < object_count; ++i) {
            // reinterpret_cast 用于将原始内存视为空闲节点
            FreeNode* node = reinterpret_cast<FreeNode*>(ptr + i * sizeof(T));
            node->next = free_list_;
            free_list_ = node;
        }
    }

    // 从全局池获取一批节点到 TLAB
    size_t fetch_from_global(size_t count) {
        std::lock_guard<std::mutex> lock(mtx_);
        
        // 如果全局池空了，先扩容
        if (!free_list_) {
            expand(std::max(count, GROW_SIZE));
        }

        size_t fetched = 0;
        // 链表摘除操作 (O(N) where N=count)
        while (free_list_ && fetched < count) {
            FreeNode* node = free_list_;
            free_list_ = node->next;
            
            node->next = t_cache_.free_list;
            t_cache_.free_list = node;
            
            fetched++;
            used_count_++;
        }
        return fetched;
    }

    // 将 TLAB 中的节点归还全局池
    // 优化：先在本地构建链表，再加锁一次性合并，最小化临界区时间。
    void return_to_global(size_t count) {
        if (count == 0 || !t_cache_.free_list) return;

        // Phase 1: Local list building (Lock-free)
        FreeNode* batch_head = nullptr;
        FreeNode* batch_tail = nullptr;
        size_t batch_size = 0;
        
        while (t_cache_.free_list && batch_size < count) {
            FreeNode* node = t_cache_.free_list;
            t_cache_.free_list = node->next;
            t_cache_.count--; // 立即更新本地计数
            
            if (!batch_head) {
                batch_head = batch_tail = node;
            } else {
                batch_tail->next = node;
                batch_tail = node;
            }
            batch_size++;
        }

        // Phase 2: Global merge (Critical Section)
        if (batch_head) {
            std::lock_guard<std::mutex> lock(mtx_);
            batch_tail->next = free_list_;
            free_list_ = batch_head;
            
            used_count_ -= batch_size;

            // 触发 GC 检查
            maintain_ops_counter_++;
            if (maintain_ops_counter_ >= MAINTAIN_INTERVAL) { 
                maintain_ops_counter_ = 0;
                maintain();
            }
        }
    }

    // 建议在线程结束时调用，清空缓存
    void flush_thread_cache() {
        if (!t_cache_.free_list) return;
        return_to_global(t_cache_.count);
        t_cache_.count = 0;
        t_cache_.free_list = nullptr;
    }

    // 维护策略：根据历史负载动态调整容量
    // 功能：更新长期峰值，并根据需要触发 shrink
    void maintain() {
        // 1. 峰值追踪与衰减 (Peak Decay)
        size_t current_usage = used_count_;
        if (current_usage > long_term_peak_) {
            long_term_peak_ = current_usage;
        } else {
            // 缓慢衰减峰值预期 (模拟遗忘因子 0.999)
            long_term_peak_ = static_cast<size_t>(long_term_peak_ * 0.999);
        }
        
        if (long_term_peak_ < min_capacity_) long_term_peak_ = min_capacity_;

        size_t target_capacity = long_term_peak_ * 1.2; // 预留 20% 余量
        if (target_capacity < min_capacity_) target_capacity = min_capacity_;

        // 2. 触发收缩
        // 仅当容量显著过剩 (1.5倍) 时触发，防止抖动。
        if (total_capacity_ > target_capacity * 1.5) {
            shrink(target_capacity);
        }
    }

    // 垃圾回收 (GC)：释放未使用的 Page
    // target_capacity: 目标保留容量，多余的空闲页将被释放
    void shrink(size_t target_capacity) {
        // 1. 标记活跃度 (Mark)
        // 遍历空闲链表确定每个 Page 的实际占用情况
        for (auto page : pages_) {
            page->active_count = page->capacity; // 默认全满
        }

        FreeNode* curr = free_list_;
        while (curr) {
            // 二分查找定位所属 Page
            auto it = std::upper_bound(pages_.begin(), pages_.end(), curr, 
                [](const void* addr, const Page* page) {
                    return addr < page->memory;
                });
            
            if (it != pages_.begin()) {
                Page* page = *(--it);
                if (page->contains(curr)) {
                    page->active_count--; // 发现一个空闲节点，活跃数减一
                }
            }
            curr = curr->next;
        }

        // 2. 筛选 (Sweep Plan)
        std::vector<Page*> pages_to_keep;
        std::vector<Page*> pages_to_free;
        
        auto now = std::chrono::steady_clock::now();

        for (auto page : pages_) {
            // 优化：增加时间检查，避免释放最近创建或活跃的页
            bool recently_active = (now - page->last_active < std::chrono::seconds(5)); // 5秒保护期

            if (page->active_count == 0 && !recently_active && (total_capacity_ - page->capacity) >= target_capacity) {
                pages_to_free.push_back(page);
                total_capacity_ -= page->capacity;
            } else {
                pages_to_keep.push_back(page);
            }
        }

        if (pages_to_free.empty()) return;

        // 3. 重建空闲链表 (Rebuild FreeList)
        FreeNode* new_free_list = nullptr;
        FreeNode** tail_ptr = &new_free_list; 

        curr = free_list_;
        while (curr) {
            FreeNode* next_node = curr->next;
            bool is_garbage = false;
            
            // 简单遍历判断，可优化
            for (auto p : pages_to_free) {
                if (p->contains(curr)) {
                    is_garbage = true;
                    break;
                }
            }

            if (!is_garbage) {
                *tail_ptr = curr;
                tail_ptr = &curr->next;
            }
            curr = next_node;
        }
        *tail_ptr = nullptr;
        free_list_ = new_free_list;

        // 4. 物理释放
        for (auto p : pages_to_free) {
            delete p;
        }
        pages_ = pages_to_keep;
    }

    template<typename... Args>
    T* allocate(Args&&... args) {
        // 快速路径：TLAB 分配
        if (!t_cache_.free_list) {
            cache_misses_++;
            fetch_from_global(t_cache_.BATCH_SIZE);
            if (!t_cache_.free_list) throw std::bad_alloc();
        } else {
            cache_hits_++;
        }

        FreeNode* node = t_cache_.free_list;
        t_cache_.free_list = node->next;
        t_cache_.count--;
        
        alloc_count_++;

        // Placement New: 在已分配的内存地址上直接构造对象
        return new (node) T(std::forward<Args>(args)...);
    }

    // 归还内存
    // ptr: 指向要释放的对象的指针
    void deallocate(T* ptr) {
        if (!ptr) return;

        ptr->~T(); // 显式调用析构函数

        // 快速路径：归还至 TLAB (无锁)
        FreeNode* node = reinterpret_cast<FreeNode*>(ptr);
        node->next = t_cache_.free_list;
        t_cache_.free_list = node;
        t_cache_.count++;
        
        free_count_++;

        // 慢速路径：检查是否触发归还 (Hysteresis Check)
        t_cache_.pending_return_count++;

        if (t_cache_.count > t_cache_.MAX_CACHE && 
            t_cache_.pending_return_count >= t_cache_.RETURN_THRESHOLD) {
            
            return_to_global(t_cache_.BATCH_SIZE);
            t_cache_.pending_return_count = 0;
        }
    }

    // 检查指针是否属于本内存池
    bool contains(T* ptr) {
        std::lock_guard<std::mutex> lock(mtx_);
        for (auto page : pages_) {
            // 计算当前页的地址范围
            // start: 页内存起始地址
            // end: 页内存结束地址
            char* start = static_cast<char*>(page->memory);
            char* end = start + (page->capacity * page->size);
            
            if (reinterpret_cast<char*>(ptr) >= start && reinterpret_cast<char*>(ptr) < end) {
                return true;
            }
        }
        return false;
    }

    // 预热内存池
    void warmup(size_t count) {
        // 临时分配一批对象以触发扩容和缓存填充
        std::vector<T*> temp;
        temp.reserve(count);
        
        for (size_t i = 0; i < count; ++i) {
            try {
                temp.push_back(allocate());
            } catch (...) {
                break;
            }
        }
        
        for (auto ptr : temp) {
            deallocate(ptr);
        }
        
        // 归还到全局池，供其他线程使用
        flush_thread_cache();
    }

    // 线程安全的调试信息转储
    void dump_debug_info() {
        std::lock_guard<std::mutex> lock(mtx_);
        
        std::cout << "=== Memory Pool Debug Info ===\n";
        std::cout << "Total Capacity: " << total_capacity_.load() << "\n";
        std::cout << "Used Count: " << used_count_.load() << "\n";
        std::cout << "Alloc Count: " << alloc_count_.load() << "\n";
        std::cout << "Free Count: " << free_count_.load() << "\n";
        std::cout << "Cache Hits: " << cache_hits_.load() << "\n";
        std::cout << "Cache Misses: " << cache_misses_.load() << "\n";
        
        std::cout << "Pages: " << pages_.size() << "\n";
        // 简单的页统计，暂时移除未使用变量 empty_pages 以消除警告
        // size_t empty_pages = 0;
        for (auto page : pages_) {
             // 注意：这里没有重新计算 active_count，只是快照
             if (now() - page->last_active < std::chrono::seconds(5)) {
                 // recently active
             }
        }
        
        // 统计全局空闲链表长度 (O(N))
        size_t global_free_nodes = 0;
        FreeNode* curr = free_list_;
        while (curr) {
            global_free_nodes++;
            curr = curr->next;
        }
        std::cout << "Global Free List Nodes: " << global_free_nodes << "\n";
        std::cout << "==============================\n";
    }
    
    std::chrono::steady_clock::time_point now() {
        return std::chrono::steady_clock::now();
    }

    //获取统计信息
    void get_stats(size_t& out_alloc, size_t& out_free, size_t& out_used, size_t& out_cap) {
        out_alloc = alloc_count_.load();
        out_free = free_count_.load();
        out_used = used_count_.load();
        out_cap = total_capacity_.load();
    }
    
    void reset_round_stats() {
        alloc_count_ = 0;
        free_count_ = 0;
    }
};

template<typename T>
thread_local ThreadCache MemoryPool<T>::t_cache_;
