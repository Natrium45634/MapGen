#pragma once

// Deterministic CPU worker pool.
//
// Work is split into fixed-size chunks whose boundaries depend only on the
// range and the grain, never on the number of threads. Every parallel loop
// writes disjoint outputs; reductions are combined chunk by chunk in index
// order, so results are bit-identical for any thread count.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace mapgen {

class CancelledError : public std::runtime_error {
public:
    CancelledError() : std::runtime_error("generation cancelled") {}
};

class CancelToken {
public:
    void cancel() { flag_.store(true, std::memory_order_relaxed); }
    void reset() { flag_.store(false, std::memory_order_relaxed); }
    bool cancelled() const { return flag_.load(std::memory_order_relaxed); }
    void check() const { if (cancelled()) throw CancelledError(); }

private:
    std::atomic<bool> flag_{false};
};

class ThreadPool {
public:
    explicit ThreadPool(int threads = 0);  // 0 = hardware concurrency
    ~ThreadPool();
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    int threadCount() const { return static_cast<int>(workers_.size()) + 1; }

    // Calls fn(chunkBegin, chunkEnd) for consecutive chunks of [begin, end).
    // The calling thread participates. Exceptions are rethrown (first by chunk index).
    void forChunks(int64_t begin, int64_t end, int64_t grain,
                   const std::function<void(int64_t, int64_t)>& fn,
                   const CancelToken* cancel = nullptr);

    // Per-row helper: fn(row) for rows [0, rows).
    void forRows(int rows, const std::function<void(int)>& fn, const CancelToken* cancel = nullptr) {
        forChunks(0, rows, 1, [&](int64_t a, int64_t b) {
            for (int64_t r = a; r < b; ++r) fn(static_cast<int>(r));
        }, cancel);
    }

    // Deterministic reduction: partial results per chunk combined in chunk order.
    template <class T, class MapFn, class CombineFn>
    T reduce(int64_t begin, int64_t end, int64_t grain, T init, MapFn map, CombineFn combine,
             const CancelToken* cancel = nullptr) {
        if (end <= begin) return init;
        int64_t chunks = (end - begin + grain - 1) / grain;
        std::vector<T> partial(static_cast<size_t>(chunks), init);
        forChunks(0, chunks, 1, [&](int64_t c0, int64_t c1) {
            for (int64_t c = c0; c < c1; ++c) {
                int64_t a = begin + c * grain;
                int64_t b = std::min(end, a + grain);
                partial[static_cast<size_t>(c)] = map(a, b);
            }
        }, cancel);
        T acc = init;
        for (auto& p : partial) acc = combine(acc, p);
        return acc;
    }

private:
    struct Job {
        const std::function<void(int64_t, int64_t)>* fn = nullptr;
        int64_t begin = 0, end = 0, grain = 1, chunks = 0;
        std::atomic<int64_t> next{0};
        std::atomic<int64_t> done{0};
        int active = 0;  // workers inside runChunks; guarded by ThreadPool::mutex_
        const CancelToken* cancel = nullptr;
        std::mutex errMutex;
        int64_t errChunk = -1;
        std::exception_ptr error;
    };
    void workerLoop();
    void runChunks(Job& job);

    std::vector<std::thread> workers_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::condition_variable doneCv_;
    Job* job_ = nullptr;
    uint64_t generation_ = 0;
    bool stop_ = false;
    std::mutex submitMutex_;
};

}  // namespace mapgen
