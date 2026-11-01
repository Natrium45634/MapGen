#include "core/util/Parallel.h"

#include <algorithm>

namespace mapgen {

namespace {
// Nested parallel loops (a parallel body calling forChunks again) run serially.
thread_local bool tInsideParallel = false;
}  // namespace

ThreadPool::ThreadPool(int threads) {
    int n = threads > 0 ? threads : static_cast<int>(std::thread::hardware_concurrency());
    n = std::clamp(n, 1, 64);
    for (int i = 0; i < n - 1; ++i) workers_.emplace_back([this] { workerLoop(); });
}

ThreadPool::~ThreadPool() {
    {
        std::lock_guard<std::mutex> lk(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    for (auto& t : workers_) t.join();
}

void ThreadPool::runChunks(Job& job) {
    while (true) {
        int64_t c = job.next.fetch_add(1);
        if (c >= job.chunks) break;
        int64_t a = job.begin + c * job.grain;
        int64_t b = std::min(job.end, a + job.grain);
        try {
            if (job.cancel && job.cancel->cancelled()) throw CancelledError();
            bool prev = tInsideParallel;
            tInsideParallel = true;
            try { (*job.fn)(a, b); } catch (...) { tInsideParallel = prev; throw; }
            tInsideParallel = prev;
        } catch (...) {
            std::lock_guard<std::mutex> lk(job.errMutex);
            if (job.errChunk < 0 || c < job.errChunk) {
                job.errChunk = c;
                job.error = std::current_exception();
            }
        }
        if (job.done.fetch_add(1) + 1 == job.chunks) {
            std::lock_guard<std::mutex> lk(mutex_);
            doneCv_.notify_all();
        }
    }
}

void ThreadPool::workerLoop() {
    uint64_t seen = 0;
    while (true) {
        Job* job = nullptr;
        {
            std::unique_lock<std::mutex> lk(mutex_);
            cv_.wait(lk, [&] { return stop_ || (job_ != nullptr && generation_ != seen); });
            if (stop_) return;
            seen = generation_;
            job = job_;
            ++job->active;
        }
        runChunks(*job);
        {
            std::lock_guard<std::mutex> lk(mutex_);
            --job->active;
        }
        doneCv_.notify_all();
    }
}

void ThreadPool::forChunks(int64_t begin, int64_t end, int64_t grain,
                           const std::function<void(int64_t, int64_t)>& fn, const CancelToken* cancel) {
    if (end <= begin) return;
    grain = std::max<int64_t>(1, grain);
    int64_t chunks = (end - begin + grain - 1) / grain;
    if (workers_.empty() || chunks == 1 || tInsideParallel) {
        for (int64_t c = 0; c < chunks; ++c) {
            if (cancel) cancel->check();
            int64_t a = begin + c * grain;
            fn(a, std::min(end, a + grain));
        }
        return;
    }
    std::lock_guard<std::mutex> submit(submitMutex_);  // nested parallel loops run serially per pool
    Job job;
    job.fn = &fn;
    job.begin = begin;
    job.end = end;
    job.grain = grain;
    job.chunks = chunks;
    job.cancel = cancel;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        job_ = &job;
        ++generation_;
    }
    cv_.notify_all();
    runChunks(job);
    {
        std::unique_lock<std::mutex> lk(mutex_);
        doneCv_.wait(lk, [&] { return job.done.load() >= job.chunks && job.active == 0; });
        job_ = nullptr;
    }
    if (job.error) std::rethrow_exception(job.error);
}

}  // namespace mapgen
