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

namespace batchpress {

/**
 * @brief High-performance thread pool.
 *
 * Header-only implementation. No platform-specific code.
 * Compiles on Linux, Windows, macOS and Android NDK.
 *
 * Thread-safety guarantees:
 *   - submit() is safe to call concurrently with shutdown()
 *   - Workers catch all exceptions — no deadlock during stack unwinding
 *   - active_ is incremented inside the lock — wait_all() never returns early
 */
class BATCHPRESS_API ThreadPool {
public:
    explicit ThreadPool(size_t num_threads = 0)
        : stop_(false), active_(0), submitted_(0), completed_(0)
    {
        // Handle platforms where hardware_concurrency() returns 0
        if (num_threads == 0)
            num_threads = std::max(1u, std::thread::hardware_concurrency());

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
            queue_.emplace([task]{ (*task)(); });
        }
        submitted_.fetch_add(1, std::memory_order_relaxed);
        cv_.notify_one();
        return future;
    }

    void wait_all() {
        std::unique_lock lock(mu_);
        drain_cv_.wait(lock, [this]{
            return queue_.empty() && active_.load(std::memory_order_acquire) == 0;
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

                // Execute with exception safety — no deadlock during stack unwinding
                try {
                    task();
                } catch (...) {
                    // Task exceptions are intentionally swallowed. The caller
                    // receives the exception via future::get() (packaged_task).
                }

                active_.fetch_sub(1, std::memory_order_acq_rel);
                completed_.fetch_add(1, std::memory_order_relaxed);
                drain_cv_.notify_all();
            }
        });
    }

    std::vector<std::thread>          workers_;
    std::queue<std::function<void()>> queue_;
    mutable std::mutex                mu_;
    std::condition_variable           cv_;
    std::condition_variable           drain_cv_;
    std::atomic<bool>                 stop_;
    std::atomic<uint32_t>             active_;
    std::atomic<uint64_t>             submitted_;
    std::atomic<uint64_t>             completed_;
};

} // namespace batchpress
