#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#ifdef _WIN32
#include <malloc.h>
#endif
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

// 可选预热内存页，减少首次访问的缺页中断
// #define MEMORY_POOL_PREHEAT

namespace memory_pool_detail {

inline std::uintptr_t address_value(const void* ptr) {
    return reinterpret_cast<std::uintptr_t>(ptr);
}

} // namespace memory_pool_detail

// Linux 对齐内存分配器
// 作用：基于 posix_memalign 提供 Docker/Linux 环境下的对齐分配接口。
class AlignedAllocator {
public:
    // 分配对齐内存
    // size: 需要分配的内存大小
    // alignment: 对齐字节数
    static void* allocate(size_t size, size_t alignment) {
#ifdef _WIN32
        void* ptr = _aligned_malloc(size, alignment);
        if (!ptr) {
            throw std::bad_alloc();
        }
        return ptr;
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
        std::free(ptr);
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
    std::string thread_label;      // 调试标签，用于区分业务线程
    size_t alloc_count = 0;        // 当前线程累计分配次数
    size_t free_count = 0;         // 当前线程累计释放次数
    size_t cache_hits = 0;         // 当前线程本地缓存命中次数
    size_t cache_misses = 0;       // 当前线程访问全局池次数
    size_t global_fetch_count = 0; // 当前线程从全局池批量获取次数
    size_t global_fetch_nodes = 0; // 当前线程从全局池获取的节点总数
    size_t global_return_count = 0; // 当前线程向全局池批量归还次数
    size_t global_return_nodes = 0; // 当前线程向全局池归还的节点总数
    size_t published_alloc_count = 0; // 已汇总到全局统计的分配次数
    size_t published_free_count = 0;  // 已汇总到全局统计的释放次数
    size_t published_cache_hits = 0;  // 已汇总到全局统计的本地命中次数
    size_t published_cache_misses = 0; // 已汇总到全局统计的全局兜底次数
    
    // ----------- 调优参数 -----------
    
    // 批量传输大小：当缓存不足或溢出时，与全局池交互的对象数量。
    // 较大的 BATCH_SIZE 可分摊锁竞争 (Lock Contention) 开销。
    const size_t BATCH_SIZE = 512; 

    // 缓存软上限：当本地缓存超过此阈值，触发归还判定逻辑。
    const size_t MAX_CACHE = 4096;

    // 缓存回落目标：触发批量归还后，本地缓存保留到该数量，避免大批量释放滞留在线程局部池。
    const size_t TARGET_CACHE = 2048;
    
    // ----------- 迟滞策略 (Hysteresis Strategy) -----------
    // 引入延迟归还机制，避免 count 在 MAX_CACHE 附近波动时引发频繁的锁操作 (Thrashing)。
    // 只有当 pending_return_count 累积到 RETURN_THRESHOLD 时，才真正执行系统级归还。
    
    size_t pending_return_count = 0;       // 累积的待归还计数
    const size_t RETURN_THRESHOLD = 1024;  // 触发归还的阈值
};

// 线程局部池调试快照
// 作用：对外暴露每个线程局部缓存的占用与全局池交互情况。
struct ThreadCacheDebugInfo {
    std::thread::id thread_id;      // 标准库线程ID
    std::string thread_label;       // 业务线程标签
    size_t local_cached_nodes = 0;  // 局部池当前缓存节点数
    size_t pending_return_nodes = 0; // 等待触发批量归还的节点计数
    size_t alloc_count = 0;         // 当前线程累计分配次数
    size_t free_count = 0;          // 当前线程累计释放次数
    size_t cache_hits = 0;          // 当前线程本地命中次数
    size_t cache_misses = 0;        // 当前线程全局兜底次数
    size_t global_fetch_count = 0;  // 当前线程全局获取批次数
    size_t global_fetch_nodes = 0;  // 当前线程全局获取节点数
    size_t global_return_count = 0; // 当前线程全局归还批次数
    size_t global_return_nodes = 0; // 当前线程全局归还节点数
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
        const auto start = memory_pool_detail::address_value(memory);
        const auto end = start + (capacity * size);
        const auto current = memory_pool_detail::address_value(ptr);
        return current >= start && current < end;
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

    // 线程局部池统计快照
    mutable std::mutex thread_stats_mtx_; // 保护线程统计快照
    std::map<std::thread::id, ThreadCacheDebugInfo> thread_cache_stats_; // 每个线程最近一次发布的局部池状态

    // 线程局部存储 (TLS) 的缓存
    static thread_local std::unordered_map<MemoryPool<T>*, ThreadCache>* t_caches_;
    static thread_local MemoryPool<T>* t_last_pool_;
    static thread_local ThreadCache* t_last_cache_;

    const size_t INITIAL_SIZE = 5120; // 初始分配大小
    const size_t GROW_SIZE = 5120;    // 每次动态扩容的数量
    
    // 维护相关的参数
    size_t min_capacity_ = 5000;
    size_t max_capacity_ = 1000000;
    size_t long_term_peak_ = 0; // 长期观察到的峰值使用量
    size_t maintain_ops_counter_ = 0; // 计数器，用于触发维护
    const size_t MAINTAIN_INTERVAL = 1000; // 每1000次deallocate检查一次

    // 获取当前线程在当前池实例上的局部缓存
    ThreadCache& thread_cache() {
        if (t_last_pool_ == this && t_last_cache_) {
            return *t_last_cache_;
        }

        if (!t_caches_) {
            t_caches_ = new std::unordered_map<MemoryPool<T>*, ThreadCache>();
        }

        ThreadCache& cache = (*t_caches_)[this];
        t_last_pool_ = this;
        t_last_cache_ = &cache;
        return cache;
    }

    // 批量发布当前线程的统计计数
    void publish_thread_cache_counters(ThreadCache& cache) {
        const size_t alloc_delta = cache.alloc_count - cache.published_alloc_count;
        const size_t free_delta = cache.free_count - cache.published_free_count;
        const size_t hit_delta = cache.cache_hits - cache.published_cache_hits;
        const size_t miss_delta = cache.cache_misses - cache.published_cache_misses;

        if (alloc_delta > 0) {
            alloc_count_.fetch_add(alloc_delta, std::memory_order_relaxed);
            cache.published_alloc_count = cache.alloc_count;
        }
        if (free_delta > 0) {
            free_count_.fetch_add(free_delta, std::memory_order_relaxed);
            cache.published_free_count = cache.free_count;
        }
        if (hit_delta > 0) {
            cache_hits_.fetch_add(hit_delta, std::memory_order_relaxed);
            cache.published_cache_hits = cache.cache_hits;
        }
        if (miss_delta > 0) {
            cache_misses_.fetch_add(miss_delta, std::memory_order_relaxed);
            cache.published_cache_misses = cache.cache_misses;
        }
    }

    // 发布当前线程局部池快照
    void publish_thread_cache_stats(ThreadCache& cache) {
        publish_thread_cache_counters(cache);

        ThreadCacheDebugInfo snapshot;
        snapshot.thread_id = std::this_thread::get_id();
        snapshot.thread_label = cache.thread_label;
        snapshot.local_cached_nodes = cache.count;
        snapshot.pending_return_nodes = cache.pending_return_count;
        snapshot.alloc_count = cache.alloc_count;
        snapshot.free_count = cache.free_count;
        snapshot.cache_hits = cache.cache_hits;
        snapshot.cache_misses = cache.cache_misses;
        snapshot.global_fetch_count = cache.global_fetch_count;
        snapshot.global_fetch_nodes = cache.global_fetch_nodes;
        snapshot.global_return_count = cache.global_return_count;
        snapshot.global_return_nodes = cache.global_return_nodes;

        std::lock_guard<std::mutex> lock(thread_stats_mtx_);
        thread_cache_stats_[snapshot.thread_id] = snapshot;
    }

public:
    MemoryPool() {
        expand(INITIAL_SIZE);
    }

    ~MemoryPool() {
        flush_thread_cache();
        if (t_caches_) {
            t_caches_->erase(this);
        }
        if (t_last_pool_ == this) {
            t_last_pool_ = nullptr;
            t_last_cache_ = nullptr;
        }
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
                return memory_pool_detail::address_value(a->memory) < memory_pool_detail::address_value(b->memory);
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
        ThreadCache& cache = thread_cache();
        size_t fetched = 0;

        {
            std::lock_guard<std::mutex> lock(mtx_);
            
            // 如果全局池空了，先扩容
            if (!free_list_) {
                expand(std::max(count, GROW_SIZE));
            }

            // 链表摘除操作 (O(N) where N=count)
            while (free_list_ && fetched < count) {
                FreeNode* node = free_list_;
                free_list_ = node->next;
                
                node->next = cache.free_list;
                cache.free_list = node;
                
                fetched++;
                cache.count++;
            }

            used_count_ += fetched;
        }

        cache.global_fetch_count++;
        cache.global_fetch_nodes += fetched;
        publish_thread_cache_stats(cache);
        return fetched;
    }

    // 将 TLAB 中的节点归还全局池
    // 优化：先在本地构建链表，再加锁一次性合并，最小化临界区时间。
    void return_to_global(size_t count) {
        ThreadCache& cache = thread_cache();
        if (count == 0 || !cache.free_list) {
            publish_thread_cache_stats(cache);
            return;
        }

        // Phase 1: Local list building (Lock-free)
        FreeNode* batch_head = nullptr;
        FreeNode* batch_tail = nullptr;
        size_t batch_size = 0;
        
        while (cache.free_list && batch_size < count) {
            FreeNode* node = cache.free_list;
            cache.free_list = node->next;
            cache.count--; // 立即更新本地计数
            
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

        cache.global_return_count++;
        cache.global_return_nodes += batch_size;
        publish_thread_cache_stats(cache);
    }

    // 建议在线程结束时调用，清空缓存
    void flush_thread_cache() {
        ThreadCache& cache = thread_cache();
        if (!cache.free_list) {
            cache.count = 0;
            cache.pending_return_count = 0;
            publish_thread_cache_stats(cache);
            return;
        }
        return_to_global(cache.count);
        cache.count = 0;
        cache.free_list = nullptr;
        cache.pending_return_count = 0;
        publish_thread_cache_stats(cache);
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
        // 遍历全局空闲链表，标记空闲节点所属的 Page
        FreeNode* curr = free_list_;
        while (curr) {
            // 二分查找定位所属 Page
            auto it = std::upper_bound(pages_.begin(), pages_.end(), curr, 
                [](const void* addr, const Page* page) {
                    return memory_pool_detail::address_value(addr) < memory_pool_detail::address_value(page->memory);
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
        // 遍历旧链表，剔除将被释放页的节点
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
        ThreadCache& cache = thread_cache();

        // 快速路径：TLAB 分配
        if (!cache.free_list) {
            cache.cache_misses++;
            fetch_from_global(cache.BATCH_SIZE);
            if (!cache.free_list) throw std::bad_alloc();
        } else {
            cache.cache_hits++;
        }

        FreeNode* node = cache.free_list;
        cache.free_list = node->next;
        cache.count--;
        cache.alloc_count++;

        // Placement New: 在已分配的内存地址上直接构造对象
        return new (node) T(std::forward<Args>(args)...);
    }

    // 归还内存
    // ptr: 指向要释放的对象的指针
    void deallocate(T* ptr) {
        if (!ptr) return;

        ThreadCache& cache = thread_cache();
        ptr->~T(); // 显式调用析构函数

        // 快速路径：归还至 TLAB (无锁)
        FreeNode* node = reinterpret_cast<FreeNode*>(ptr);
        node->next = cache.free_list;
        cache.free_list = node;
        cache.count++;
        cache.free_count++;

        // 慢速路径：大批量释放时将多余节点一次性归还全局池，避免释放线程长期囤积缓存。
        cache.pending_return_count++;

        if (cache.count > cache.MAX_CACHE && 
            cache.pending_return_count >= cache.RETURN_THRESHOLD) {
            const size_t return_count = cache.count > cache.TARGET_CACHE
                ? cache.count - cache.TARGET_CACHE
                : 0;
            cache.pending_return_count = 0;
            return_to_global(return_count);
        }
    }

    // 批量直接归还全局池
    // 适用于 Manager、Cleaner 等集中回收线程，避免跨线程释放的节点滞留在回收线程局部池。
    template<typename Iterator>
    void deallocate_batch_to_global(Iterator begin, Iterator end) {
        ThreadCache& cache = thread_cache();
        FreeNode* batch_head = nullptr;
        FreeNode* batch_tail = nullptr;
        size_t batch_size = 0;

        // ---- 1. 本地析构并构建待归还链表 ----
        for (Iterator it = begin; it != end; ++it) {
            T* ptr = *it;
            if (!ptr) continue;

            ptr->~T();
            FreeNode* node = reinterpret_cast<FreeNode*>(ptr);
            node->next = nullptr;

            if (!batch_head) {
                batch_head = batch_tail = node;
            } else {
                batch_tail->next = node;
                batch_tail = node;
            }
            batch_size++;
        }

        if (batch_size == 0) {
            publish_thread_cache_stats(cache);
            return;
        }

        // ---- 2. 一次性合并到全局空闲链表 ----
        {
            std::lock_guard<std::mutex> lock(mtx_);
            batch_tail->next = free_list_;
            free_list_ = batch_head;
            used_count_ -= batch_size;

            maintain_ops_counter_++;
            if (maintain_ops_counter_ >= MAINTAIN_INTERVAL) {
                maintain_ops_counter_ = 0;
                maintain();
            }
        }

        cache.free_count += batch_size;
        cache.global_return_count++;
        cache.global_return_nodes += batch_size;
        publish_thread_cache_stats(cache);
    }

    // 检查指针是否属于本内存池
    bool contains(T* ptr) {
        std::lock_guard<std::mutex> lock(mtx_);
        for (auto page : pages_) {
            // 计算当前页的地址范围
            // start: 页内存起始地址
            // end: 页内存结束地址
            const auto start = memory_pool_detail::address_value(page->memory);
            const auto end = start + (page->capacity * page->size);
            const auto current = memory_pool_detail::address_value(ptr);
            
            if (current >= start && current < end) {
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
        const std::vector<ThreadCacheDebugInfo> thread_snapshots = get_thread_cache_stats();
        size_t page_count = 0;
        size_t global_free_nodes = 0;

        {
            std::lock_guard<std::mutex> lock(mtx_);
            page_count = pages_.size();

            // 统计全局空闲链表长度 (O(N))
            FreeNode* curr = free_list_;
            while (curr) {
                global_free_nodes++;
                curr = curr->next;
            }
        }
        
        std::cout << "=== Memory Pool Debug Info ===\n";
        std::cout << "Total Capacity: " << total_capacity_.load() << "\n";
        std::cout << "Used Count: " << used_count_.load() << "\n";
        std::cout << "Alloc Count: " << alloc_count_.load() << "\n";
        std::cout << "Free Count: " << free_count_.load() << "\n";
        std::cout << "Cache Hits: " << cache_hits_.load() << "\n";
        std::cout << "Cache Misses: " << cache_misses_.load() << "\n";
        std::cout << "Pages: " << page_count << "\n";
        std::cout << "Global Free List Nodes: " << global_free_nodes << "\n";
        std::cout << "Thread Local Pools:\n";
        if (thread_snapshots.empty()) {
            std::cout << "  <none>\n";
        }
        for (const ThreadCacheDebugInfo& snapshot : thread_snapshots) {
            const size_t active_delta = snapshot.alloc_count >= snapshot.free_count
                ? snapshot.alloc_count - snapshot.free_count
                : 0;
            const std::string label = snapshot.thread_label.empty() ? "unnamed" : snapshot.thread_label;
            std::cout << "  Thread " << snapshot.thread_id
                      << " [" << label << "]"
                      << " | Local Cached: " << snapshot.local_cached_nodes
                      << " | Active Delta: " << active_delta
                      << " | Pending Return: " << snapshot.pending_return_nodes
                      << " | Alloc/Free: " << snapshot.alloc_count << "/" << snapshot.free_count
                      << " | Hit/Miss: " << snapshot.cache_hits << "/" << snapshot.cache_misses
                      << " | Global Fetch: " << snapshot.global_fetch_count << " batches, " << snapshot.global_fetch_nodes << " nodes"
                      << " | Global Return: " << snapshot.global_return_count << " batches, " << snapshot.global_return_nodes << " nodes"
                      << "\n";
        }
        std::cout << "==============================\n";
    }
    
    std::chrono::steady_clock::time_point now() {
        return std::chrono::steady_clock::now();
    }

    //获取统计信息
    void get_stats(size_t& out_alloc, size_t& out_free, size_t& out_used, size_t& out_cap) {
        ThreadCache& cache = thread_cache();
        publish_thread_cache_counters(cache);

        out_alloc = alloc_count_.load();
        out_free = free_count_.load();
        out_used = used_count_.load();
        out_cap = total_capacity_.load();
    }

    // 设置当前线程的调试标签
    void set_current_thread_label(const std::string& label) {
        ThreadCache& cache = thread_cache();
        cache.thread_label = label;
        publish_thread_cache_stats(cache);
    }

    // 获取当前线程局部池快照
    ThreadCacheDebugInfo get_current_thread_cache_stats() {
        ThreadCache& cache = thread_cache();
        publish_thread_cache_stats(cache);

        std::lock_guard<std::mutex> lock(thread_stats_mtx_);
        return thread_cache_stats_[std::this_thread::get_id()];
    }

    // 获取全部线程局部池快照
    std::vector<ThreadCacheDebugInfo> get_thread_cache_stats() const {
        std::vector<ThreadCacheDebugInfo> snapshots;
        std::lock_guard<std::mutex> lock(thread_stats_mtx_);
        snapshots.reserve(thread_cache_stats_.size());
        for (const auto& item : thread_cache_stats_) {
            snapshots.push_back(item.second);
        }
        return snapshots;
    }
    
    void reset_round_stats() {
        alloc_count_ = 0;
        free_count_ = 0;
    }
};

template<typename T>
thread_local std::unordered_map<MemoryPool<T>*, ThreadCache>* MemoryPool<T>::t_caches_ = nullptr;

template<typename T>
thread_local MemoryPool<T>* MemoryPool<T>::t_last_pool_ = nullptr;

template<typename T>
thread_local ThreadCache* MemoryPool<T>::t_last_cache_ = nullptr;
