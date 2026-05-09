#include "MemoryPool.hpp"

#include <atomic>
#include <cassert>
#include <string>
#include <thread>
#include <utility>
#include <vector>

struct TestObject {
    int id;
    std::string name;
    char payload[32]{};

    TestObject() : id(0), name("default") {}
    TestObject(int object_id, std::string object_name) : id(object_id), name(std::move(object_name)) {}
};

// ---- 基础分配释放测试 ----
void test_allocate_and_deallocate() {
    MemoryPool<TestObject> pool;

    TestObject* object = pool.allocate(7, "bullet");
    assert(object != nullptr);
    assert(object->id == 7);
    assert(object->name == "bullet");
    assert(pool.contains(object));

    pool.deallocate(object);
    pool.flush_thread_cache();
}

// ---- 批量分配释放测试 ----
void test_batch_allocate_and_deallocate() {
    MemoryPool<TestObject> pool;
    std::vector<TestObject*> objects;
    objects.reserve(2048);

    for (int index = 0; index < 2048; ++index) {
        objects.push_back(pool.allocate(index, "batch"));
    }

    for (TestObject* object : objects) {
        assert(object != nullptr);
        assert(pool.contains(object));
        pool.deallocate(object);
    }

    pool.flush_thread_cache();

    size_t alloc_count = 0;
    size_t free_count = 0;
    size_t used_count = 0;
    size_t total_capacity = 0;
    pool.get_stats(alloc_count, free_count, used_count, total_capacity);

    assert(alloc_count == 2048);
    assert(free_count == 2048);
    assert(total_capacity >= 2048);
}

// ---- 预热测试 ----
void test_warmup() {
    MemoryPool<TestObject> pool;
    pool.warmup(1024);

    size_t alloc_count = 0;
    size_t free_count = 0;
    size_t used_count = 0;
    size_t total_capacity = 0;
    pool.get_stats(alloc_count, free_count, used_count, total_capacity);

    assert(total_capacity >= 1024);
}

// ---- 多线程分配释放测试 ----
void test_multithread_allocate_and_deallocate() {
    MemoryPool<TestObject> pool;
    std::atomic<int> completed_threads{0};
    std::vector<std::thread> workers;

    for (int thread_id = 0; thread_id < 4; ++thread_id) {
        workers.emplace_back([&pool, &completed_threads, thread_id]() {
            std::vector<TestObject*> local_objects;
            local_objects.reserve(512);

            for (int index = 0; index < 512; ++index) {
                local_objects.push_back(pool.allocate(thread_id * 1000 + index, "worker"));
            }

            for (TestObject* object : local_objects) {
                assert(object != nullptr);
                pool.deallocate(object);
            }

            pool.flush_thread_cache();
            ++completed_threads;
        });
    }

    for (std::thread& worker : workers) {
        worker.join();
    }

    assert(completed_threads == 4);
}

// ---- 线程局部池统计测试 ----
void test_thread_local_cache_stats() {
    MemoryPool<TestObject> pool;
    std::atomic<int> completed_threads{0};
    std::vector<std::thread> workers;

    for (int thread_id = 0; thread_id < 2; ++thread_id) {
        workers.emplace_back([&pool, &completed_threads, thread_id]() {
            pool.set_current_thread_label("test-worker-" + std::to_string(thread_id));

            std::vector<TestObject*> local_objects;
            local_objects.reserve(128);

            for (int index = 0; index < 128; ++index) {
                local_objects.push_back(pool.allocate(thread_id * 1000 + index, "thread-local"));
            }

            for (TestObject* object : local_objects) {
                pool.deallocate(object);
            }

            ThreadCacheDebugInfo before_flush = pool.get_current_thread_cache_stats();
            assert(before_flush.thread_label == "test-worker-" + std::to_string(thread_id));
            assert(before_flush.local_cached_nodes > 0);
            assert(before_flush.global_fetch_nodes >= 128);

            pool.flush_thread_cache();

            ThreadCacheDebugInfo after_flush = pool.get_current_thread_cache_stats();
            assert(after_flush.local_cached_nodes == 0);
            assert(after_flush.global_return_nodes >= before_flush.local_cached_nodes);

            ++completed_threads;
        });
    }

    for (std::thread& worker : workers) {
        worker.join();
    }

    std::vector<ThreadCacheDebugInfo> snapshots = pool.get_thread_cache_stats();
    size_t matched_workers = 0;
    for (const ThreadCacheDebugInfo& snapshot : snapshots) {
        if (snapshot.thread_label.find("test-worker-") == 0) {
            ++matched_workers;
        }
    }

    assert(completed_threads == 2);
    assert(matched_workers == 2);
}

// ---- 批量直接归还全局池测试 ----
void test_batch_deallocate_to_global() {
    MemoryPool<TestObject> pool;
    std::vector<TestObject*> objects;
    objects.reserve(1024);

    for (int index = 0; index < 1024; ++index) {
        objects.push_back(pool.allocate(index, "batch-global"));
    }

    ThreadCacheDebugInfo before_return = pool.get_current_thread_cache_stats();
    pool.deallocate_batch_to_global(objects.begin(), objects.end());
    ThreadCacheDebugInfo after_return = pool.get_current_thread_cache_stats();

    size_t alloc_count = 0;
    size_t free_count = 0;
    size_t used_count = 0;
    size_t total_capacity = 0;
    pool.get_stats(alloc_count, free_count, used_count, total_capacity);

    assert(alloc_count == 1024);
    assert(free_count == 1024);
    assert(after_return.global_return_nodes >= before_return.global_return_nodes + 1024);
    assert(used_count <= after_return.local_cached_nodes);

    pool.flush_thread_cache();
}

// ---- 局部池回落策略测试 ----
void test_thread_cache_trim_after_bulk_free() {
    MemoryPool<TestObject> pool;
    std::vector<TestObject*> objects;
    objects.reserve(8192);

    for (int index = 0; index < 8192; ++index) {
        objects.push_back(pool.allocate(index, "trim"));
    }

    for (TestObject* object : objects) {
        pool.deallocate(object);
    }

    ThreadCacheDebugInfo snapshot = pool.get_current_thread_cache_stats();
    assert(snapshot.local_cached_nodes <= 4096);
    assert(snapshot.global_return_nodes > 0);

    pool.flush_thread_cache();
}

// ---- 测试入口 ----
int main() {
    test_allocate_and_deallocate();
    test_batch_allocate_and_deallocate();
    test_warmup();
    test_multithread_allocate_and_deallocate();
    test_thread_local_cache_stats();
    test_batch_deallocate_to_global();
    test_thread_cache_trim_after_bulk_free();
    return 0;
}
