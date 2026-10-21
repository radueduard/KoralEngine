//
// kphysics: a pool of threads for the step's parallel parts. Made for many small jobs in quick succession (a solver pass
// is a job a colour, a few hundred a step): the threads spin between jobs for a little while, so handing one over costs
// about a microsecond, and sleep when no job has come for that while (between steps), so an idle pool costs nothing.
//
// The caller works too: a pool of n threads is the caller and n − 1 workers. A job may ask for fewer than all of them;
// each worker it asks for takes part in it (it may find nothing left to take), which is what lets the job's description
// be plain fields, reused job after job.
//

#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

namespace kphysics
{
    class ThreadPool {
    public:
        /** @brief @p threads in all, the caller among them; 0 is as many as the machine has cores, up to 8. */
        explicit ThreadPool(int threads = 0);
        ~ThreadPool();
        ThreadPool(const ThreadPool&) = delete;
        ThreadPool& operator=(const ThreadPool&) = delete;

        [[nodiscard]] int Size() const { return static_cast<int>(_workers.size()) + 1; }

        /**
         * @brief @p body(begin, end) over [0, count), in chunks of @p grain taken by whichever of @p threads threads is
         *        free (the caller among them; 0 is all of the pool's); returns when every chunk is done. Fewer than two
         *        chunks, or one thread: on the caller alone. The others go on waiting for a job that is theirs.
         */
        template <typename F> void For(int count, int grain, F&& body, int threads = 0);

    private:
        void Run(void (*call)(void*, int, int), void* context, int count, int grain, int threads);
        void Work(int index);
        void Chunks();

        std::vector<std::thread> _workers;
        // The job: what to call, on what, over how much.
        void (*_call)(void*, int, int) = nullptr;
        void* _context = nullptr;
        int _count = 0, _grain = 1;
        alignas(64) std::atomic<int> _next { 0 };           // the next chunk's start
        alignas(64) std::atomic<int> _finished { 0 };       // workers done with this job
        /// The job: its number (it goes up with each) times 256, plus how many threads take part. Workers wait on it.
        alignas(64) std::atomic<std::uint32_t> _job { 0 };
        std::atomic<bool> _stop { false };
    };

    template <typename F>
    void ThreadPool::For(const int count, const int grain, F&& body, int threads)
    {
        if (count <= 0) return;
        threads = threads <= 0 ? Size() : std::min(threads, Size());
        if (threads == 1 || count <= grain) { body(0, count); return; }
        Run([](void* context, const int begin, const int end) { (*static_cast<F*>(context))(begin, end); }, &body, count, grain, threads);
    }
}
