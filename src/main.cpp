#include "../include/MemoryPool.hpp"
#include <vector>
#include <string>
#include <sstream>
#include <thread>
#include <iostream>
#include <algorithm>
#include <deque>
#include <map>
#include <condition_variable>

// 全局调试控制
std::atomic<bool> g_debug_mode{false};

// 线程安全的打印控制
std::mutex g_console_mtx;
void log(const std::string& msg) {
    std::lock_guard<std::mutex> lock(g_console_mtx);
    std::cout << msg << std::endl;
}

// 模拟子弹对象
struct Bullet {
    int owner_id; // 发射该子弹的线程ID
    std::chrono::steady_clock::time_point launch_time;
    char padding[32]; // 模拟 payload

    Bullet(int id) : owner_id(id) {
        launch_time = std::chrono::steady_clock::now();
    }
};

MemoryPool<Bullet> g_pool;

// 全局子弹管理器：负责生命周期监控与自动回收
class BulletManager {
private:
    std::deque<Bullet*> active_bullets_;
    std::mutex mtx_;
    std::thread cleaner_thread_;
    std::atomic<bool> running_{true};
    const std::chrono::seconds LIFETIME{30}; // 生命周期 30秒

public:
    BulletManager() {
        // 启动后台清理线程
        cleaner_thread_ = std::thread([this]() {
            while (running_) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100)); // 10Hz 检查频率
                process_expiration();
            }
        });
    }

    ~BulletManager() {
        running_ = false;
        if (cleaner_thread_.joinable()) cleaner_thread_.join();
    }

    // 注册新子弹
    void add(Bullet* b) {
        std::lock_guard<std::mutex> lock(mtx_);
        active_bullets_.push_back(b); // 因时间单调递增，队尾总是最新的，队头是最老的
    }

    // 清理所有（用于 clear 命令）
    void clear_all() {
        std::lock_guard<std::mutex> lock(mtx_);
        size_t count = active_bullets_.size();
        for (auto* b : active_bullets_) {
            g_pool.deallocate(b);
        }
        active_bullets_.clear();
        
        size_t alloc, free_cnt, used, cap;
        g_pool.get_stats(alloc, free_cnt, used, cap);
        log("[Manager] Force cleared " + std::to_string(count) + " bullets. (Pool Used: " + std::to_string(used) + "/" + std::to_string(cap) + ")");
    }

    // 检查过期子弹
    void process_expiration() {
        std::lock_guard<std::mutex> lock(mtx_);
        auto now = std::chrono::steady_clock::now();
        
        int cleaned_count = 0;
        // 检查队头，如果过期则回收，直到遇到未过期的（因为是有序的）
        while (!active_bullets_.empty()) {
            Bullet* b = active_bullets_.front();
            if (now - b->launch_time >= LIFETIME) {
                active_bullets_.pop_front();
                g_pool.deallocate(b); // 回收内存
                cleaned_count++;
            } else {
                break; // 队头都没过期，后面的肯定也没过期
            }
        }
        
        if (cleaned_count > 0) {
            size_t alloc, free_cnt, used, cap;
            g_pool.get_stats(alloc, free_cnt, used, cap);
            log("[Manager] Auto-recycled " + std::to_string(cleaned_count) + " old bullets. (Pool Used: " + std::to_string(used) + "/" + std::to_string(cap) + ")");
        }
    }
    
    size_t get_active_count() {
        std::lock_guard<std::mutex> lock(mtx_);
        return active_bullets_.size();
    }
};

BulletManager g_manager;

// 持久化工作线程
class Worker {
private:
    int id_;
    std::thread thread_;
    std::deque<int> tasks_; // 任务队列：存储需要生成的子弹数量
    std::mutex mtx_;
    std::condition_variable cv_;
    std::atomic<bool> running_{true};

public:
    Worker(int id) : id_(id) {
        thread_ = std::thread([this]() {
            while (running_) {
                int alloc_num = 0;
                {
                    std::unique_lock<std::mutex> lock(mtx_);
                    cv_.wait(lock, [this]() { return !tasks_.empty() || !running_; });
                    
                    if (!running_ && tasks_.empty()) return;
                    
                    alloc_num = tasks_.front();
                    tasks_.pop_front();
                }

                // 特殊任务：-1 代表强制刷新缓存
                if (alloc_num == -1) {
                    g_pool.flush_thread_cache();
                    continue;
                }

                long long ordinary_time_us = 0;
                long long pure_pool_time_us = 0; // 新增：纯内存池基准
                long long pool_time_us = 0;

                // Debug模式下：先跑一遍普通分配的基准测试 (模拟)
                if (g_debug_mode) {
                    // 1. 基准测试：普通 New/Delete
                    auto start = std::chrono::high_resolution_clock::now();
                    for (int i = 0; i < alloc_num; ++i) {
                        Bullet* b = new Bullet(id_);
                        delete b; 
                    }
                    auto end = std::chrono::high_resolution_clock::now();
                    ordinary_time_us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();

                    // 2. 基准测试：纯内存池 Alloc/Dealloc (公平对比)
                    // 这能反映出剥离掉 g_manager 锁和队列操作后的真实分配速度
                    start = std::chrono::high_resolution_clock::now();
                    for (int i = 0; i < alloc_num; ++i) {
                        Bullet* b = g_pool.allocate(id_);
                        g_pool.deallocate(b);
                    }
                    end = std::chrono::high_resolution_clock::now();
                    pure_pool_time_us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
                }

                // 执行真实任务：使用内存池 (含 Manager 锁和队列操作)
                auto p_start = std::chrono::high_resolution_clock::now();
                for (int i = 0; i < alloc_num; ++i) {
                    Bullet* b = g_pool.allocate(id_); // 使用 MemoryPool 的参数转发
                    g_manager.add(b);
                }
                auto p_end = std::chrono::high_resolution_clock::now();
                pool_time_us = std::chrono::duration_cast<std::chrono::microseconds>(p_end - p_start).count();
                
                if (g_debug_mode) {
                    std::string msg = "[Debug] Thread " + std::to_string(id_) 
                                    + " | Count: " + std::to_string(alloc_num)
                                    + " | Ordinary: " + std::to_string(ordinary_time_us) + " us"
                                    + " | Pure Pool: " + std::to_string(pure_pool_time_us) + " us"
                                    + " | Real(incl. Logic): " + std::to_string(pool_time_us) + " us";
                    log(msg);
                } else {
                    size_t alloc, free_cnt, used, cap;
                    g_pool.get_stats(alloc, free_cnt, used, cap);
                    std::string msg = "[Thread " + std::to_string(id_) + "] Fired " + std::to_string(alloc_num) + " bullets. (Pool Used: " + std::to_string(used) + "/" + std::to_string(cap) + ")";
                    log(msg);
                }
            }
        });
    }

    ~Worker() {
        running_ = false;
        cv_.notify_all();
        if (thread_.joinable()) thread_.join();
    }

    void add_task(int count) {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            tasks_.push_back(count);
        }
        cv_.notify_one();
    }
};

// 全局 Worker 注册表
std::map<int, std::shared_ptr<Worker>> g_workers;

int main() {
    log("=== High Performance Memory Pool System ===");
    log("Commands:");
    log("  <ThreadID> <Count>  : Thread ID fires Count bullets (e.g., '1 5')");
    log("  clear               : Force recycle all active bullets");
    log("  status              : Show pool stats");
    log("  debug               : Toggle debug mode (benchmark info)");
    log("  exit                : Quit");
    log("===========================================");

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line == "exit") break;
        if (line.empty()) continue;

        if (line == "debug") {
            bool current = g_debug_mode;
            g_debug_mode = !current;
            log(std::string("[System] Debug mode ") + (g_debug_mode ? "ENABLED" : "DISABLED"));
            continue;
        }

        if (line == "clear") {
            g_manager.clear_all();
            
            // 刷新所有工作线程的本地缓存
            // 因为工作线程持有私有的 TLAB，简单 clear 只能回收 "Managed" 对象，
            // 无法触及缓存在每个线程本地 freelist 中的节点。
            // 我们不能直接操作其他线程的 thread_local 变量，所以必须通知它们自己去做。
            // 这里通过发送特殊任务代码 -1 来触发 Worker 内部的 flush。
            for (auto& w : g_workers) {
                w.second->add_task(-1);
            }
            
            // 刷新主线程缓存
            g_pool.flush_thread_cache();
            
            g_pool.reset_round_stats(); // 重置计数器以便观察后续变化
            
            // 提示用户异步清理正在进行
            log("[System] Flush signal sent to all workers. Pool stats will update shortly.");
            continue;
        }

        if (line == "status") {
            size_t alloc, free_cnt, used, cap;
            g_pool.get_stats(alloc, free_cnt, used, cap);
            size_t active_managed = g_manager.get_active_count();
            
            std::cout << "--- Pool Status ---\n"
                      << "Active (Managed): " << active_managed << "\n"
                      << "Total Generated : " << alloc << "\n"
                      << "Total Recycled  : " << free_cnt << "\n"
                      << "Pool Used/Cap   : " << used << " / " << cap << "\n"
                      << "-------------------" << std::endl;
            continue;
        }

        std::stringstream ss(line);
        int thread_id, count;
        if (ss >> thread_id >> count) {
            // 查找或创建线程
            if (g_workers.find(thread_id) == g_workers.end()) {
                g_workers[thread_id] = std::make_shared<Worker>(thread_id);
                log("[System] Created new Worker Thread " + std::to_string(thread_id));
            }
            
            // 下发任务
            g_workers[thread_id]->add_task(count);
        } else if (line == "flush") {
             // 新增隐藏指令：通知所有线程刷新缓存
             // 实际上需要更复杂的线程间通信。
             // 这里仅作为占位，并未真正实现全线程 flush。
             g_pool.flush_thread_cache();
             log("[System] Main thread cache flushed.");
        } else {
            log("[Error] Invalid format. Use: <ThreadID> <Count>");
        }
    }
    
    // 退出前清理
    g_workers.clear(); // 析构所有 Worker
    g_manager.clear_all(); 
    
    return 0;
}
