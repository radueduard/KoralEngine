#include <kphysics/thread_pool.h>

#include <algorithm>
#include <chrono>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#define KPHYSICS_PAUSE() _mm_pause()
#else
#define KPHYSICS_PAUSE() std::this_thread::yield()
#endif

namespace kphysics
{
    namespace
    {
        /** How long a worker spins for the next job before it sleeps: far longer than between two jobs of a step. */
        constexpr auto SpinFor = std::chrono::microseconds(300);
    }

    ThreadPool::ThreadPool(int threads)
    {
        if (threads <= 0) threads = static_cast<int>(std::clamp(std::thread::hardware_concurrency(), 1u, 8u));
        threads = std::min(threads, 255);                   // how many a job takes is a byte of its word
        for (int i = 1; i < threads; ++i) _workers.emplace_back([this, i] { Work(i - 1); });
    }

    ThreadPool::~ThreadPool()
    {
        _stop.store(true, std::memory_order_release);
        _job.store(((_job.load(std::memory_order_relaxed) >> 8) + 1) << 8 | 0xFFu, std::memory_order_release);   // every worker
        _job.notify_all();
        for (std::thread& worker : _workers) worker.join();
    }

    void ThreadPool::Run(void (*call)(void*, int, int), void* context, const int count, const int grain, const int threads)
    {
        _call = call;
        _context = context;
        _count = count;
        _grain = grain;
        _next.store(0, std::memory_order_relaxed);
        _finished.store(0, std::memory_order_relaxed);
        // The job's number and how many threads take part, in one word, so that a worker reads both alike; publishes the
        // fields above to whoever sees it.
        const std::uint32_t job = ((_job.load(std::memory_order_relaxed) >> 8) + 1) << 8 | static_cast<std::uint32_t>(threads);
        _job.store(job, std::memory_order_release);
        _job.notify_all();
        Chunks();
        const int workers = threads - 1;                    // the caller is one of them
        while (_finished.load(std::memory_order_acquire) != workers) KPHYSICS_PAUSE();
    }

    void ThreadPool::Chunks()
    {
        for (;;) {
            const int begin = _next.fetch_add(_grain, std::memory_order_relaxed);
            if (begin >= _count) return;
            _call(_context, begin, std::min(begin + _grain, _count));
        }
    }

    void ThreadPool::Work(const int index)
    {
        // Job 0 is the pool as it was made, not the job now: a worker whose thread starts only after the first job was
        // handed out must still see that job as new, or the caller waits for it for ever.
        std::uint32_t seen = 0;
        for (;;) {
            // Spin a while for the next job; then sleep until there is one.
            const auto until = std::chrono::steady_clock::now() + SpinFor;
            std::uint32_t now = _job.load(std::memory_order_acquire);
            for (int spins = 0; now == seen; ++spins) {
                KPHYSICS_PAUSE();
                if ((spins & 255) == 255 && std::chrono::steady_clock::now() > until) {
                    _job.wait(seen, std::memory_order_acquire);
                }
                now = _job.load(std::memory_order_acquire);
            }
            seen = now;
            if (_stop.load(std::memory_order_acquire)) return;
            // Not one of this job's threads: nothing of it is this worker's, not even its fields.
            if (index >= static_cast<int>(now & 0xFFu) - 1) continue;
            Chunks();
            _finished.fetch_add(1, std::memory_order_release);
        }
    }
}
