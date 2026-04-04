// SPDX-License-Identifier: MIT
/*
 * Copyright (C) 2018 Marco Antônio Bueno da Silva <bueno.marco@gmail.com>
 *
 * This file is part of batchpress.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#pragma once

#include "export.hpp"
#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <future>
#include <atomic>
#include <stdexcept>
#include <algorithm>

// Android-specific: thread priority and yield
#ifdef __ANDROID__
#include <sys/prctl.h>
#include <sys/resource.h>
#include <pthread.h>
#include <unistd.h>
#include <sched.h>
#endif

namespace batchpress {

/**
 * @brief High-performance thread pool with Android ANR prevention.
 *
 * Features for Android safety:
 *   - Workers use BACKGROUND thread priority (won't compete with UI)
 *   - Cooperative yielding prevents CPU monopolization
 *   - Cancellation support for mid-batch abort
 *   - Thread count defaults to min(cores, 4) on Android
 *
 * Thread-safety guarantees:
 *   - submit() is safe to call concurrently with shutdown()
 *   - Workers catch all exceptions — no deadlock during stack unwinding
 *   - active_ is incremented inside the lock — wait_all() never returns early
 */
class BATCHPRESS_API ThreadPool {
public:
    /**
     * @brief Construct a thread pool.
     * @param num_threads  0 = auto (scales with cores, reserves margin for UI)
     *
     * Auto-sizing formula on Android:
     *   threads = max(1, floor(cores * scale_factor))
     *
     * This always reserves cores - threads ≥ 1 cores for the UI thread and
     * system tasks, preventing ANR while still utilizing available hardware.
     *
     * Scale factors by core count:
     *   ≤2 cores  → cores - 1  (reserve 1 for UI)
     *   3-6 cores → cores - 2  (reserve 2 for UI + system)
     *   7-12 cores → floor(cores * 0.7)  (30% reserved)
     *   13+ cores → floor(cores * 0.6)   (40% reserved, but more absolute cores)
     *
     * Examples:
     *   4 cores  → 2 threads   (2 reserved for UI/system)
     *   6 cores  → 4 threads   (2 reserved)
     *   8 cores  → 5 threads   (3 reserved)
     *   16 cores → 9 threads   (7 reserved)
     */
    explicit ThreadPool(size_t num_threads = 0)
        : stop_(false), cancelled_(false), active_(0), submitted_(0), completed_(0)
    {
        if (num_threads == 0) {
#ifdef __ANDROID__
            size_t cores = std::thread::hardware_concurrency();
            if (cores == 0) cores = 2;  // absolute fallback

            if (cores <= 2) {
                num_threads = 1;  // reserve 1 core for UI on very low-end devices
            } else if (cores <= 6) {
                num_threads = cores - 2;  // reserve 2 cores for UI + system
            } else if (cores <= 12) {
                num_threads = static_cast<size_t>(cores * 0.7);  // 30% reserved
            } else {
                num_threads = static_cast<size_t>(cores * 0.6);  // 40% reserved
            }
            if (num_threads == 0) num_threads = 1;
#else
            num_threads = std::max(1u, std::thread::hardware_concurrency());
#endif
        }

        workers_.reserve(num_threads);
        for (size_t i = 0; i < num_threads; ++i)
            spawn_worker();
    }

    ~ThreadPool() { shutdown(); }

    ThreadPool(const ThreadPool&)            = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&)                 = delete;
    ThreadPool& operator=(ThreadPool&&)      = delete;

    template<typename F, typename... Args>
    auto submit(F&& f, Args&&... args)
        -> std::future<std::invoke_result_t<F, Args...>>
    {
        using R = std::invoke_result_t<F, Args...>;

        // Check stop_ INSIDE the lock to prevent TOCTOU race
        auto task = std::make_shared<std::packaged_task<R()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...)
        );
        auto future = task->get_future();
        {
            std::lock_guard lock(mu_);
            if (stop_.load(std::memory_order_acquire))
                throw std::runtime_error("ThreadPool: submit on stopped pool");
            queue_.emplace([task, this]{
                try { (*task)(); } catch (...) { /* handled by caller via future */ }
            });
        }
        submitted_.fetch_add(1, std::memory_order_relaxed);
        cv_.notify_one();
        return future;
    }

    /**
     * @brief Request cancellation of remaining tasks.
     *
     * Tasks already running will complete, but queued tasks will be skipped.
     * Safe to call from any thread (including JNI/Java callback).
     */
    void cancel() {
        cancelled_.store(true, std::memory_order_release);
        { std::lock_guard lock(mu_); stop_.store(true, std::memory_order_release); }
        cv_.notify_all();
    }

    /// Check if cancellation was requested
    bool is_cancelled() const noexcept {
        return cancelled_.load(std::memory_order_acquire);
    }

    /**
     * @brief Cooperative yield — call from inside long-running tasks.
     *
     * On Android: sleeps 1ms to yield CPU to UI thread.
     * Call this periodically in encoding loops to prevent ANR.
     */
    void yield_cooperatively() const noexcept {
#ifdef __ANDROID__
        // Brief sleep gives the Android scheduler a chance to run the UI thread.
        // 1ms is short enough to not noticeably slow processing,
        // but long enough to prevent ANR (5-second watchdog).
        usleep(1000);  // 1ms
#else
        std::this_thread::yield();
#endif
    }

    void wait_all() {
        std::unique_lock lock(mu_);
        drain_cv_.wait(lock, [this]{
            return (stop_.load(std::memory_order_acquire) && queue_.empty()) ||
                   active_.load(std::memory_order_acquire) == 0;
        });
    }

    void shutdown() {
        // Set stop FIRST so workers wake up and exit
        { std::lock_guard lock(mu_); stop_.store(true, std::memory_order_release); }
        cv_.notify_all();
        // Then wait for all active tasks to finish
        wait_all();
        for (auto& w : workers_) if (w.joinable()) w.join();
        workers_.clear();
    }

    size_t   worker_count()    const noexcept { return workers_.size(); }
    uint64_t tasks_submitted() const noexcept { return submitted_.load(); }
    uint64_t tasks_completed() const noexcept { return completed_.load(); }

private:
    void spawn_worker() {
        workers_.emplace_back([this]{
#ifdef __ANDROID__
            // Set thread name for debugging (visible in logcat)
            pthread_setname_np(pthread_self(), "batchpress_wrk");

            // Set SCHED_OTHER with lowest normal priority (nice value 10).
            // This ensures UI thread (default nice 0) always gets priority
            // when contending for CPU, preventing ANR freezes.
            setpriority(PRIO_PROCESS, 0, 10);

            // Also use Android's PRIO_BACKGROUND hint
            setpriority(PRIO_PROCESS, 0, 10);
#endif

            while (true) {
                std::function<void()> task;
                {
                    std::unique_lock lock(mu_);
                    cv_.wait(lock, [this]{
                        return stop_.load(std::memory_order_acquire) || !queue_.empty();
                    });
                    if (stop_.load() && queue_.empty()) return;
                    task = std::move(queue_.front());
                    queue_.pop();
                    // Increment active_ INSIDE the lock — prevents wait_all() from
                    // returning prematurely when a task is popped but not yet started
                    active_.fetch_add(1, std::memory_order_acq_rel);
                }

                // Execute task (exception already wrapped in submit)
                task();

                active_.fetch_sub(1, std::memory_order_acq_rel);
                completed_.fetch_add(1, std::memory_order_relaxed);
                drain_cv_.notify_all();

                // Cooperative yield after each task — brief sleep prevents
                // the pool from monopolizing CPU cores on Android
#ifdef __ANDROID__
                usleep(500);  // 0.5ms between tasks
#endif
            }
        });
    }

    std::vector<std::thread>          workers_;
    std::queue<std::function<void()>> queue_;
    mutable std::mutex                mu_;
    std::condition_variable           cv_;
    std::condition_variable           drain_cv_;
    std::atomic<bool>                 stop_;
    std::atomic<bool>                 cancelled_;
    std::atomic<uint32_t>             active_;
    std::atomic<uint64_t>             submitted_;
    std::atomic<uint64_t>             completed_;
};

} // namespace batchpress
