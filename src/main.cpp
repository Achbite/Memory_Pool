#include "MemoryPool.hpp"

#include <atomic>
#include <memory>
#include <vector>
#include <string>
#include <sstream>
#include <thread>
#include <iostream>
#include <algorithm>
#include <deque>
#include <map>
#include <condition_variable>
#include <chrono>
#include <filesystem>
#include <fstream>

// 自动压测配置
constexpr int kAutoTestBatchCount = 10;             // 自动压测批次数
constexpr int kAutoTestBulletCount = 100000;        // 每批每个 Worker 访问的对象数量
constexpr int kAutoTestWorkerCount = 2;             // 自动压测使用的工作线程数
const char* const kAutoTestLogPath = "log/test_log.log"; // 自动压测日志路径

// 防止基准测试中的对象访问被优化器消除
std::atomic<long long> g_benchmark_checksum{0};

// 全局调试控制
// 控制是否开启基准测试模式。开启后，工作线程会运行对比测试（系统分配 vs 内存池）。
std::atomic<bool> g_debug_mode{false};
std::atomic<bool> g_auto_test_sync_enabled{false}; // 控制自动压测 worker 同步起跑

// 线程安全的打印控制
// 用于防止多线程同时输出到控制台导致字符错乱。
std::mutex g_console_mtx;
void log(const std::string& msg) {
    std::lock_guard<std::mutex> lock(g_console_mtx);
    std::cout << msg << std::endl;
}

// 模拟子弹对象
// 这是一个典型的小对象，用于模拟游戏中高频创建和销毁的实体。
struct Bullet {
    int owner_id; // 发射该子弹的线程ID，用于追踪对象来源
    std::chrono::steady_clock::time_point launch_time; // 发射时间，用于生命周期管理（超时销毁）
    char padding[32]; // 填充数据，模拟实际业务对象的 Payload 大小

    Bullet(int id) : owner_id(id) {
        launch_time = std::chrono::steady_clock::now();
    }
};

// 全局内存池实例
// 专门为 Bullet 类型实例化的内存池，所有 Worker 线程共享此实例。
MemoryPool<Bullet> g_pool;

// ---- 大批量内存读写 ----
long long touch_bullet(Bullet* bullet, int worker_id, int index) {
    const size_t padding_index = static_cast<size_t>(index) % sizeof(bullet->padding);
    bullet->owner_id = worker_id;
    bullet->padding[padding_index] = static_cast<char>((worker_id + index) & 0x7F);
    return bullet->owner_id + bullet->padding[padding_index];
}

// 全局子弹管理器：负责生命周期监控与自动回收
// 模拟游戏引擎中的 "Object Manager" 或服务器中的 "Session Manager"。
// 它持有所有活跃对象的指针，并定期检查它们是否过期。
class BulletManager {
private:
    std::deque<Bullet*> active_bullets_; // 活跃对象队列。由于按时间顺序插入，天然有序。
    std::mutex mtx_; // 保护 active_bullets_ 的并发访问
    std::thread cleaner_thread_; // 后台清理线程 handle
    std::atomic<bool> running_{true}; // 控制清理线程退出的标志
    std::atomic<int> lifetime_seconds_{30}; // 对象存活时间，默认30秒

public:
    BulletManager() {
        // 启动后台清理线程
        // 以 10Hz 的频率（每100ms）执行一次过期检查
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
    // 将由 MemoryPool 分配的对象纳入管理。
    // b: 指向新对象的指针
    void add(Bullet* b) {
        std::lock_guard<std::mutex> lock(mtx_);
        active_bullets_.push_back(b); // 因时间单调递增，队尾总是最新的，队头是最老的
    }

    // 设置对象生命周期
    // 自动压测可缩短生命周期，避免等待默认30秒。
    void set_lifetime(std::chrono::seconds lifetime) {
        lifetime_seconds_.store(static_cast<int>(lifetime.count()), std::memory_order_relaxed);
    }

    // 清理所有（用于 clear 命令）
    // 强制回收所有活跃对象，通常用于重置场景或压力测试后的清理。
    void clear_all() {
        std::deque<Bullet*> recycled_bullets;
        {
            std::lock_guard<std::mutex> lock(mtx_);
            recycled_bullets.swap(active_bullets_);
        }

        const size_t count = recycled_bullets.size();
        g_pool.deallocate_batch_to_global(recycled_bullets.begin(), recycled_bullets.end());
        
        size_t alloc, free_cnt, used, cap;
        g_pool.get_stats(alloc, free_cnt, used, cap);
        log("[Manager] Force cleared " + std::to_string(count) + " bullets. (Pool Used: " + std::to_string(used) + "/" + std::to_string(cap) + ")");
    }

    // 检查过期子弹
    // 这是主要的自动回收逻辑。
    // 利用队列的时间有序性，只需要检查队头元素即可，效率极高。
    void process_expiration() {
        std::deque<Bullet*> expired_bullets;
        auto now = std::chrono::steady_clock::now();
        const std::chrono::seconds lifetime(lifetime_seconds_.load(std::memory_order_relaxed));
        
        {
            std::lock_guard<std::mutex> lock(mtx_);
            // 检查队头，如果过期则摘出，直到遇到未过期的（因为是有序的）
            while (!active_bullets_.empty()) {
                Bullet* b = active_bullets_.front();
                if (now - b->launch_time >= lifetime) {
                    active_bullets_.pop_front();
                    expired_bullets.push_back(b);
                } else {
                    break; // 队头都没过期，后面的肯定也没过期，无需继续遍历
                }
            }
        }
        
        if (!expired_bullets.empty()) {
            g_pool.deallocate_batch_to_global(expired_bullets.begin(), expired_bullets.end());
            size_t alloc, free_cnt, used, cap;
            g_pool.get_stats(alloc, free_cnt, used, cap);
            log("[Manager] Auto-recycled " + std::to_string(expired_bullets.size()) + " old bullets. (Pool Used: " + std::to_string(used) + "/" + std::to_string(cap) + ")");
        }
    }
    
    // 获取当前活跃对象数量
    size_t get_active_count() {
        std::lock_guard<std::mutex> lock(mtx_);
        return active_bullets_.size();
    }

    // 获取每个生产线程的活跃对象数量
    std::map<int, size_t> get_active_count_by_owner() {
        std::map<int, size_t> active_count_by_owner;
        std::lock_guard<std::mutex> lock(mtx_);
        for (const Bullet* bullet : active_bullets_) {
            active_count_by_owner[bullet->owner_id]++;
        }
        return active_count_by_owner;
    }
};

BulletManager g_manager;

// ---- 自动压测批次屏障 ----
class AutoTestBatchBarrier {
private:
    std::mutex mtx_;
    std::condition_variable cv_;
    int expected_workers_ = 0;
    int ready_workers_ = 0;
    bool batch_open_ = false;

public:
    void configure(int expected_workers) {
        std::lock_guard<std::mutex> lock(mtx_);
        expected_workers_ = expected_workers;
        ready_workers_ = 0;
        batch_open_ = false;
    }

    void wait_until_all_ready() {
        std::unique_lock<std::mutex> lock(mtx_);
        cv_.wait(lock, [this]() { return ready_workers_ >= expected_workers_; });
    }

    void release_batch() {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            batch_open_ = true;
        }
        cv_.notify_all();
    }

    void wait_for_release() {
        std::unique_lock<std::mutex> lock(mtx_);
        ready_workers_++;
        if (ready_workers_ >= expected_workers_) {
            cv_.notify_all();
        }
        cv_.wait(lock, [this]() { return batch_open_; });
    }
};

AutoTestBatchBarrier g_auto_test_barrier;

// ---- 工作线程任务模式 ----
enum class WorkerTaskMode {
    ManagedLifecycle, // Manager 生命周期：对象交给管理器统一持有与释放
    ImmediateRelease  // 即时释放：Worker 在线程内申请并立即释放对象
};

struct WorkerTask {
    int count = 0; // 本次任务需要处理的对象数量
    WorkerTaskMode mode = WorkerTaskMode::ManagedLifecycle; // 本次任务的生命周期模式
};

// 持久化工作线程
// 模拟业务系统中的工作线程（Worker Thread）。
// 负责响应命令并执行对象分配任务。
class Worker {
private:
    int id_; // 线程ID
    std::thread thread_;
    std::deque<WorkerTask> tasks_; // 任务队列：存储对象数量和生命周期模式
    std::mutex mtx_;
    std::condition_variable cv_; // 用于任务到达时的通知
    std::condition_variable idle_cv_; // 用于自动压测等待任务完成
    std::atomic<bool> running_{true};
    bool busy_ = false;

public:
    Worker(int id) : id_(id) {
        thread_ = std::thread([this]() {
            g_pool.set_current_thread_label("worker-" + std::to_string(id_));

            while (running_) {
                WorkerTask task;
                {
                    // 经典的生产者-消费者模型等待
                    std::unique_lock<std::mutex> lock(mtx_);
                    cv_.wait(lock, [this]() { return !tasks_.empty() || !running_; });
                    
                    if (!running_ && tasks_.empty()) {
                        g_pool.flush_thread_cache();
                        return;
                    }
                    
                    task = tasks_.front();
                    tasks_.pop_front();
                    busy_ = true;
                }

                if (g_auto_test_sync_enabled && task.count > 0) {
                    g_auto_test_barrier.wait_for_release();
                }

                // 特殊任务：-1 代表强制刷新缓存
                // 当主线程需要回收所有资源时，会发送此信号让工作线程交出本地缓存。
                if (task.count == -1) {
                    g_pool.flush_thread_cache();
                    {
                        std::lock_guard<std::mutex> lock(mtx_);
                        busy_ = false;
                    }
                    idle_cv_.notify_all();
                    continue;
                }

                const int alloc_num = task.count;
                long long ordinary_time_us = 0;
                long long pure_pool_time_us = 0; // 纯内存池基准
                long long pool_time_us = 0;

                // Debug模式下：执行对齐后的普通分配与内存池分配基准。
                if (g_debug_mode) {
                    long long checksum = 0;

                    // ---- 1. 普通堆分配基准 ----
                    // 与内存池保持同样的“批量分配、访问对象、批量释放”负载。
                    std::vector<std::unique_ptr<Bullet>> ordinary_bullets;
                    ordinary_bullets.reserve(alloc_num);
                    auto start = std::chrono::high_resolution_clock::now();
                    for (int i = 0; i < alloc_num; ++i) {
                        ordinary_bullets.push_back(std::make_unique<Bullet>(id_));
                        checksum += touch_bullet(ordinary_bullets.back().get(), id_, i);
                    }
                    for (int i = 0; i < alloc_num; ++i) {
                        Bullet* bullet = ordinary_bullets[i].get();
                        checksum += bullet->owner_id + bullet->padding[i % sizeof(bullet->padding)];
                    }
                    ordinary_bullets.clear();
                    auto end = std::chrono::high_resolution_clock::now();
                    ordinary_time_us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();

                    // ---- 2. 纯内存池分配基准 ----
                    // 剥离 Manager 锁和队列操作，只比较同样批量生命周期下的对象分配释放。
                    std::vector<Bullet*> pool_bullets;
                    pool_bullets.reserve(alloc_num);
                    start = std::chrono::high_resolution_clock::now();
                    for (int i = 0; i < alloc_num; ++i) {
                        Bullet* b = g_pool.allocate(id_);
                        checksum += touch_bullet(b, id_, i);
                        pool_bullets.push_back(b);
                    }
                    for (int i = 0; i < alloc_num; ++i) {
                        Bullet* bullet = pool_bullets[i];
                        checksum += bullet->owner_id + bullet->padding[i % sizeof(bullet->padding)];
                    }
                    g_pool.deallocate_batch_to_global(pool_bullets.begin(), pool_bullets.end());
                    end = std::chrono::high_resolution_clock::now();
                    pure_pool_time_us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
                    g_benchmark_checksum.fetch_add(checksum, std::memory_order_relaxed);
                }

                // 执行真实任务：根据压测模式选择 Manager 生命周期或 Worker 即时释放。
                std::vector<Bullet*> worker_batch;
                worker_batch.reserve(alloc_num);
                long long real_checksum = 0;
                auto p_start = std::chrono::steady_clock::now();
                for (int i = 0; i < alloc_num; ++i) {
                    Bullet* b = g_pool.allocate(id_); // 使用 MemoryPool 的参数转发
                    real_checksum += touch_bullet(b, id_, i);
                    worker_batch.push_back(b);
                }
                for (int i = 0; i < alloc_num; ++i) {
                    Bullet* bullet = worker_batch[i];
                    real_checksum += bullet->owner_id + bullet->padding[i % sizeof(bullet->padding)];
                    if (task.mode == WorkerTaskMode::ManagedLifecycle) {
                        g_manager.add(bullet);
                    } else {
                        g_pool.deallocate(bullet);
                    }
                }
                g_benchmark_checksum.fetch_add(real_checksum, std::memory_order_relaxed);
                auto p_end = std::chrono::steady_clock::now();
                pool_time_us = std::chrono::duration_cast<std::chrono::microseconds>(p_end - p_start).count();
                
                if (g_debug_mode) {
                    ThreadCacheDebugInfo thread_stats = g_pool.get_current_thread_cache_stats();
                    const size_t active_delta = thread_stats.alloc_count >= thread_stats.free_count
                        ? thread_stats.alloc_count - thread_stats.free_count
                        : 0;
                    const std::string mode_name = task.mode == WorkerTaskMode::ManagedLifecycle
                        ? "ManagedLifecycle"
                        : "ImmediateRelease";
                    std::string msg = "[Debug] Thread " + std::to_string(id_) 
                                    + " | Mode: " + mode_name
                                    + " | Count: " + std::to_string(alloc_num)
                                    + " | Ordinary: " + std::to_string(ordinary_time_us) + " us"
                                    + " | Pure Pool: " + std::to_string(pure_pool_time_us) + " us"
                                    + " | Real(incl. Logic): " + std::to_string(pool_time_us) + " us"
                                    + " | Local Cached: " + std::to_string(thread_stats.local_cached_nodes)
                                    + " | Active Delta: " + std::to_string(active_delta)
                                    + " | Global Fetch Nodes: " + std::to_string(thread_stats.global_fetch_nodes)
                                    + " | Global Return Nodes: " + std::to_string(thread_stats.global_return_nodes);
                    log(msg);
                } else {
                    size_t alloc, free_cnt, used, cap;
                    g_pool.get_stats(alloc, free_cnt, used, cap);
                    std::string msg = "[Thread " + std::to_string(id_) + "] Fired " + std::to_string(alloc_num) + " bullets. (Pool Used: " + std::to_string(used) + "/" + std::to_string(cap) + ")";
                    log(msg);
                }

                {
                    std::lock_guard<std::mutex> lock(mtx_);
                    busy_ = false;
                }
                idle_cv_.notify_all();
            }

            g_pool.flush_thread_cache();
        });
    }

    ~Worker() {
        running_ = false;
        cv_.notify_all();
        if (thread_.joinable()) thread_.join();
    }

    // 添加任务到队列
    // count: 要发射的子弹数量
    void add_task(int count, WorkerTaskMode mode = WorkerTaskMode::ManagedLifecycle) {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            tasks_.push_back({count, mode});
        }
        cv_.notify_one();
    }

    // 等待任务队列清空且当前任务完成
    bool wait_until_idle(std::chrono::seconds timeout) {
        std::unique_lock<std::mutex> lock(mtx_);
        return idle_cv_.wait_for(lock, timeout, [this]() {
            return tasks_.empty() && !busy_;
        });
    }
};

// 全局 Worker 注册表
std::map<int, std::shared_ptr<Worker>> g_workers;

// ---- 等待全部 Worker 完成当前批次 ----
bool wait_for_all_workers_idle(std::chrono::seconds timeout) {
    for (auto& worker : g_workers) {
        if (!worker.second->wait_until_idle(timeout)) {
            return false;
        }
    }
    return true;
}

// ---- 等待业务对象数量达到目标值 ----
bool wait_for_active_count(size_t expected_count, std::chrono::seconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (g_manager.get_active_count() == expected_count) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return g_manager.get_active_count() == expected_count;
}

// ---- 等待业务对象全部回收 ----
bool wait_for_empty_manager(std::chrono::seconds timeout) {
    return wait_for_active_count(0, timeout);
}

// ---- 输出自动压测状态 ----
void log_auto_test_status(const std::string& title) {
    size_t alloc, free_cnt, used, cap;
    g_pool.get_stats(alloc, free_cnt, used, cap);
    log("[AutoTest] " + title);
    log("[AutoTest] Active Managed: " + std::to_string(g_manager.get_active_count()));
    log("[AutoTest] Pool Used/Cap: " + std::to_string(used) + "/" + std::to_string(cap));
    log("[AutoTest] Total Generated/Recycled: " + std::to_string(alloc) + "/" + std::to_string(free_cnt));
    g_pool.dump_debug_info();
}

// ---- 执行自动压测 ----
int run_auto_test() {
    std::filesystem::create_directories("log");
    std::ofstream log_file(kAutoTestLogPath, std::ios::out | std::ios::trunc);
    if (!log_file.is_open()) {
        std::cerr << "[AutoTest] Cannot open log file: " << kAutoTestLogPath << std::endl;
        return 1;
    }

    std::streambuf* cout_buffer = std::cout.rdbuf(log_file.rdbuf());
    std::streambuf* cerr_buffer = std::cerr.rdbuf(log_file.rdbuf());

    g_pool.set_current_thread_label("main-test");
    g_manager.set_lifetime(std::chrono::seconds(60));
    g_debug_mode = true;
    g_auto_test_sync_enabled = true;

    log("=== Memory Pool Auto Benchmark ===");
    log("[AutoTest] Log Path: " + std::string(kAutoTestLogPath));
    log("[AutoTest] Worker Count: " + std::to_string(kAutoTestWorkerCount));
    log("[AutoTest] Batch Count: " + std::to_string(kAutoTestBatchCount));
    log("[AutoTest] Bullet Count Per Worker Per Batch: " + std::to_string(kAutoTestBulletCount));

    for (int worker_id = 1; worker_id <= kAutoTestWorkerCount; ++worker_id) {
        g_workers[worker_id] = std::make_shared<Worker>(worker_id);
    }

    const size_t per_batch_active = static_cast<size_t>(kAutoTestWorkerCount) * kAutoTestBulletCount;

    log("[AutoTest] Scenario 1: ManagedLifecycle continuous allocation x" + std::to_string(kAutoTestBatchCount));
    g_manager.set_lifetime(std::chrono::seconds(60));
    for (int batch = 1; batch <= kAutoTestBatchCount; ++batch) {
        g_auto_test_barrier.configure(kAutoTestWorkerCount);

        log("[AutoTest][ManagedLifecycle] Batch " + std::to_string(batch) + "/" + std::to_string(kAutoTestBatchCount) + " dispatching.");
        for (int worker_id = 1; worker_id <= kAutoTestWorkerCount; ++worker_id) {
            g_workers[worker_id]->add_task(kAutoTestBulletCount, WorkerTaskMode::ManagedLifecycle);
        }

        g_auto_test_barrier.wait_until_all_ready();
        log("[AutoTest][ManagedLifecycle] Batch " + std::to_string(batch) + " workers ready, releasing together.");
        g_auto_test_barrier.release_batch();

        if (!wait_for_all_workers_idle(std::chrono::seconds(30))) {
            log("[AutoTest] ERROR: timeout waiting for managed lifecycle worker batch completion.");
            log_auto_test_status("Timeout ManagedLifecycle Batch " + std::to_string(batch) + " Worker Completion");
            std::cout.rdbuf(cout_buffer);
            std::cerr.rdbuf(cerr_buffer);
            return 2;
        }

        const size_t expected_active = per_batch_active * static_cast<size_t>(batch);
        if (!wait_for_active_count(expected_active, std::chrono::seconds(10))) {
            log("[AutoTest] ERROR: timeout waiting for managed lifecycle generated bullets.");
            log_auto_test_status("Timeout ManagedLifecycle Batch " + std::to_string(batch) + " After Generate");
            std::cout.rdbuf(cout_buffer);
            std::cerr.rdbuf(cerr_buffer);
            return 3;
        }

        log_auto_test_status("ManagedLifecycle Batch " + std::to_string(batch) + " After Generate");
    }

    log("[AutoTest] Scenario 1 cleanup: force clearing managed bullets.");
    g_manager.clear_all();
    if (!wait_for_empty_manager(std::chrono::seconds(10))) {
        log("[AutoTest] ERROR: timeout waiting for managed lifecycle cleanup.");
        log_auto_test_status("Timeout ManagedLifecycle Cleanup");
        std::cout.rdbuf(cout_buffer);
        std::cerr.rdbuf(cerr_buffer);
        return 4;
    }
    log_auto_test_status("ManagedLifecycle After Cleanup");

    log("[AutoTest] Scenario 2: ImmediateRelease allocation/free x" + std::to_string(kAutoTestBatchCount));
    for (int batch = 1; batch <= kAutoTestBatchCount; ++batch) {
        g_auto_test_barrier.configure(kAutoTestWorkerCount);

        log("[AutoTest][ImmediateRelease] Batch " + std::to_string(batch) + "/" + std::to_string(kAutoTestBatchCount) + " dispatching.");
        for (int worker_id = 1; worker_id <= kAutoTestWorkerCount; ++worker_id) {
            g_workers[worker_id]->add_task(kAutoTestBulletCount, WorkerTaskMode::ImmediateRelease);
        }

        g_auto_test_barrier.wait_until_all_ready();
        log("[AutoTest][ImmediateRelease] Batch " + std::to_string(batch) + " workers ready, releasing together.");
        g_auto_test_barrier.release_batch();

        if (!wait_for_all_workers_idle(std::chrono::seconds(30))) {
            log("[AutoTest] ERROR: timeout waiting for immediate release worker batch completion.");
            log_auto_test_status("Timeout ImmediateRelease Batch " + std::to_string(batch) + " Worker Completion");
            std::cout.rdbuf(cout_buffer);
            std::cerr.rdbuf(cerr_buffer);
            return 5;
        }

        if (!wait_for_empty_manager(std::chrono::seconds(10))) {
            log("[AutoTest] ERROR: immediate release scenario should not create managed bullets.");
            log_auto_test_status("Timeout ImmediateRelease Batch " + std::to_string(batch) + " Manager Empty Check");
            std::cout.rdbuf(cout_buffer);
            std::cerr.rdbuf(cerr_buffer);
            return 6;
        }

        log_auto_test_status("ImmediateRelease Batch " + std::to_string(batch) + " After Worker Release");
    }

    for (auto& worker : g_workers) {
        worker.second->add_task(-1);
    }
    wait_for_all_workers_idle(std::chrono::seconds(10));
    g_pool.flush_thread_cache();
    log_auto_test_status("After Flush");

    g_workers.clear();
    g_manager.clear_all();
    g_auto_test_sync_enabled = false;
    log("[AutoTest] Benchmark finished.");

    std::cout.rdbuf(cout_buffer);
    std::cerr.rdbuf(cerr_buffer);
    return 0;
}

int main(int argc, char* argv[]) {
    if (argc > 1 && std::string(argv[1]) == "test") {
        return run_auto_test();
    }

    g_pool.set_current_thread_label("main");

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
            // 因为工作线程持有私有的 TLAB (Thread Local Allocation Buffer)，简单 clear 只能回收 "Managed" 对象，
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
             
             // 如果开启了 Debug 模式，显示更详细信息         
             if (g_debug_mode) {
                 std::map<int, size_t> active_by_owner = g_manager.get_active_count_by_owner();
                 std::cout << "Active Managed By Producer Thread:\n";
                 if (active_by_owner.empty()) {
                     std::cout << "  <none>\n";
                 }
                 for (const auto& item : active_by_owner) {
                     std::cout << "  Thread " << item.first << " | Active Managed: " << item.second << "\n";
                 }
                 g_pool.dump_debug_info();
             }
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
