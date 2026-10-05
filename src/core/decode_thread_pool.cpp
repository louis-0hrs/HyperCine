// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

#include "hypercine/decode_phase2d.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

namespace HyperCine {

// Persistent bounded worker pool (Phase 2D Sections 5-7).
//
// Invariants:
//  * Threads are created only in Initialize() and joined only in Shutdown().
//  * ExecuteBatch() does not return until EVERY worker has acknowledged and
//    left the batch, even when the batch is aborted. Callers may therefore
//    pass stack-allocated task functors/state safely.
//  * Tasks are claimed dynamically through one atomic counter (no per-task
//    allocation, no shared append structures).
//  * The first exception thrown by a task is captured, remaining unclaimed
//    tasks are skipped, and the exception is rethrown on the calling thread.
struct DecoderWorkerPool::Impl {
    int num_workers = 0;
    std::vector<std::thread> workers;

    std::mutex submit_mutex;   // serializes ExecuteBatch / Initialize / Shutdown
    std::mutex mutex;          // protects batch state below
    std::condition_variable cv_work;
    std::condition_variable cv_done;

    bool shutdown = false;
    uint64_t current_batch_id = 0;
    int num_tasks = 0;
    int workers_finished = 0;  // workers that have left the current batch
    const std::function<void(int, int)>* current_task_fn = nullptr;
    std::atomic<bool>* external_abort = nullptr;

    std::atomic<int> next_task_idx{0};
    std::atomic<bool> internal_abort{false};
    std::exception_ptr first_exception;

    bool Aborted() const {
        return internal_abort.load(std::memory_order_relaxed) ||
               (external_abort && external_abort->load(std::memory_order_relaxed));
    }

    void WorkerLoop(int worker_id) {
        uint64_t seen_batch = 0;
        for (;;) {
            const std::function<void(int, int)>* fn = nullptr;
            int task_count = 0;
            {
                std::unique_lock<std::mutex> lock(mutex);
                cv_work.wait(lock, [&] { return shutdown || current_batch_id != seen_batch; });
                if (shutdown) return;
                seen_batch = current_batch_id;
                fn = current_task_fn;
                task_count = num_tasks;
            }

            while (!Aborted()) {
                const int idx = next_task_idx.fetch_add(1, std::memory_order_relaxed);
                if (idx >= task_count) break;
                try {
                    (*fn)(idx, worker_id);
                } catch (...) {
                    std::lock_guard<std::mutex> lk(mutex);
                    if (!first_exception) first_exception = std::current_exception();
                    internal_abort.store(true, std::memory_order_relaxed);
                }
            }

            {
                std::lock_guard<std::mutex> lock(mutex);
                ++workers_finished;
                if (workers_finished == num_workers) cv_done.notify_one();
            }
        }
    }

    void StopLocked() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (workers.empty()) return;
            shutdown = true;
        }
        cv_work.notify_all();
        for (auto& w : workers) {
            if (w.joinable()) w.join();
        }
        workers.clear();
        num_workers = 0;
        shutdown = false;
    }
};

DecoderWorkerPool::DecoderWorkerPool() : impl_(std::make_unique<Impl>()) {}

DecoderWorkerPool::~DecoderWorkerPool() {
    Shutdown();
}

DecoderWorkerPool::DecoderWorkerPool(DecoderWorkerPool&&) noexcept = default;
DecoderWorkerPool& DecoderWorkerPool::operator=(DecoderWorkerPool&&) noexcept = default;

bool DecoderWorkerPool::Initialize(int num_workers) {
    if (!impl_) impl_ = std::make_unique<Impl>();
    std::lock_guard<std::mutex> submit(impl_->submit_mutex);

    if (num_workers <= 0) {
        unsigned int hw = std::thread::hardware_concurrency();
        if (hw == 0) hw = 4;
        // 27 VLC bands exist, but useful parallelism saturates well below that
        // (critical path = largest band). Clamp to 16 to avoid oversubscription.
        num_workers = static_cast<int>((std::min)(hw, 16u));
    }

    if (impl_->num_workers == num_workers && !impl_->workers.empty()) return true;

    impl_->StopLocked();

    try {
        impl_->workers.reserve(static_cast<size_t>(num_workers));
        impl_->num_workers = num_workers;
        for (int i = 0; i < num_workers; ++i) {
            impl_->workers.emplace_back(&Impl::WorkerLoop, impl_.get(), i);
        }
    } catch (...) {
        impl_->num_workers = static_cast<int>(impl_->workers.size());
        impl_->StopLocked();
        return false;
    }
    return true;
}

void DecoderWorkerPool::Shutdown() {
    if (!impl_) return;
    std::lock_guard<std::mutex> submit(impl_->submit_mutex);
    impl_->StopLocked();
}

bool DecoderWorkerPool::IsRunning() const {
    return impl_ && !impl_->workers.empty();
}

int DecoderWorkerPool::WorkerCount() const {
    return impl_ ? impl_->num_workers : 0;
}

void DecoderWorkerPool::ExecuteBatch(
    int num_tasks,
    const std::function<void(int task_idx, int worker_id)>& task_fn,
    std::atomic<bool>* abort_flag
) {
    if (!impl_ || num_tasks <= 0) return;
    std::lock_guard<std::mutex> submit(impl_->submit_mutex);

    if (impl_->workers.empty()) {
        // No pool: run inline on the caller (still honors abort + exceptions).
        for (int i = 0; i < num_tasks; ++i) {
            if (abort_flag && abort_flag->load(std::memory_order_relaxed)) break;
            task_fn(i, 0);
        }
        return;
    }

    std::exception_ptr ex;
    {
        std::unique_lock<std::mutex> lock(impl_->mutex);
        impl_->next_task_idx.store(0, std::memory_order_relaxed);
        impl_->internal_abort.store(false, std::memory_order_relaxed);
        impl_->first_exception = nullptr;
        impl_->num_tasks = num_tasks;
        impl_->workers_finished = 0;
        impl_->current_task_fn = &task_fn;
        impl_->external_abort = abort_flag;
        ++impl_->current_batch_id;
        impl_->cv_work.notify_all();

        // Full quiescence: every worker must leave the batch before return.
        impl_->cv_done.wait(lock, [&] { return impl_->workers_finished == impl_->num_workers; });

        impl_->current_task_fn = nullptr;
        impl_->external_abort = nullptr;
        ex = impl_->first_exception;
        impl_->first_exception = nullptr;
    }
    if (ex) std::rethrow_exception(ex);
}

} // namespace HyperCine
