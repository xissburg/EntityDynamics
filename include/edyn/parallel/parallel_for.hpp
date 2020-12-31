#ifndef EDYN_PARALLEL_PARALLEL_FOR_HPP
#define EDYN_PARALLEL_PARALLEL_FOR_HPP

#include <mutex>
#include <atomic>
#include <numeric>
#include <iterator>
#include <forward_list>
#include "edyn/config/config.h"
#include "edyn/parallel/mutex_counter.hpp"
#include "edyn/parallel/job.hpp"
#include "edyn/parallel/job_dispatcher.hpp"
#include "edyn/serialization/memory_archive.hpp"

namespace edyn {

namespace detail {

/**
 * A for-loop with an atomic editable range.
 */
template<typename IndexType, typename Function>
struct for_loop_range {
    using atomic_index_type = std::atomic<IndexType>;

    for_loop_range(IndexType first, IndexType last, 
                   IndexType step, const Function *func,
                   mutex_counter &counter)
        : m_first(first)
        , m_last(last)
        , m_step(step)
        , m_current(first)
        , m_func(func)
        , m_counter(&counter)
    {}

    /**
     * Executes the for-loop. Increments the atomic `m_current` on each iteration.
     * Decrements the counter given in the constructor once finished.
     */
    void run() {
        auto i = m_current.load(std::memory_order_relaxed);
        while (i < m_last.load(std::memory_order_acquire)) {
            (*m_func)(i);
            i = m_current.fetch_add(m_step, std::memory_order_release) + m_step;
        }
        m_counter->decrement();
    }

    atomic_index_type m_first;
    atomic_index_type m_last;
    atomic_index_type m_step;
    atomic_index_type m_current;
    const Function *m_func;
    mutex_counter *m_counter;
};

/**
 * A pool of for-loops where new loops can be created by stealing a
 * range from another loop in the pool.
 */
template<typename IndexType, typename Function>
class for_loop_range_pool {
public:
    using for_loop_range_type = for_loop_range<IndexType, Function>;

    for_loop_range_pool(IndexType first, IndexType last, 
                        IndexType step, const Function *func)
        : m_first(first)
        , m_last(last)
        , m_step(step)
        , m_func(func)
    {}

    for_loop_range_type *steal() {
        // Key observation: this function is never called in parallel because
        // each parallel-for job spawns another parallel-for job and this 
        // function is called before a new job of that kind is scheduled.
        if (m_loops.empty()) {
            // Create the first loop with the entire range.
            m_counter.increment();
            auto &loop = m_loops.emplace_front(m_first, m_last, m_step, m_func, m_counter);
            return &loop;
        }

        IndexType candidate_remaining = 0;
        for_loop_range_type *candidate_loop = nullptr;

        // Find loop with the largest number of remaining work.
        for (auto &curr_loop : m_loops) {
            auto last = curr_loop.m_last.load(std::memory_order_acquire);
            auto current = curr_loop.m_current.load(std::memory_order_acquire);
            
            if (current > last) continue;

            auto remaining = last - current;

            if (remaining >= candidate_remaining) {
                candidate_loop = &curr_loop;
                candidate_remaining = remaining;
            }
        }

        auto last = candidate_loop->m_last.load(std::memory_order_acquire);
        auto current = candidate_loop->m_current.load(std::memory_order_acquire);

        // No work left to be stolen.
        if (!(current + 1 < last)) { 
            return nullptr;
        }

        auto remaining = last - current;
        auto first = candidate_loop->m_first.load(std::memory_order_relaxed);
        auto total = last - first;

        // Return null if the loop with the biggest amount of remaining work is
        // nearly done (i.e. the remaining work is under a percentage of the total)
        if (remaining * 100 < total * 10) {
            return nullptr;
        }

        // Increment loop counter. Will be decremented by the new loop when
        // it finishes running. It is important to increment it before range
        // stealing below, since it might terminate the `candidate_loop` right
        // after and cause `m_counter` to be decremented to zero thus causing
        // a wait on the counter to return prematurely resulting in an incomplete
        // run of the entire for-loop range.
        m_counter.increment();

        // Effectively steal a range of work from candidate by moving its last
        // index to the halfway point between current and last.
        auto middle = current + remaining / 2;
        candidate_loop->m_last.store(middle, std::memory_order_release);

        // It is possible that by the time `middle` is stored in `candidate_loop->m_last`,
        // `candidate_loop->m_current` is greater than `middle` since the for-loop
        // is running while this range stealing is taking place. To prevent calling
        // `m_func` more than once for the elements between `middle` and 
        // `candidate_loop->m_current`, load `candidate_loop->m_current` and check
        // if it's greater than or equals to `middle` and if so, start from there instead.
        current = candidate_loop->m_current.load(std::memory_order_acquire);
        auto new_first = current >= middle ? current : middle;

        auto &loop = m_loops.emplace_front(new_first, last, m_step, m_func, m_counter);
        return &loop;
    }

    void wait() {
        m_counter.wait();
    }

private:
    const IndexType m_first;
    const IndexType m_last;
    const IndexType m_step;
    const Function *m_func;
    std::forward_list<for_loop_range_type> m_loops;
    mutex_counter m_counter;
};

template<typename IndexType, typename Function>
struct parallel_for_context {
    using for_loop_range_pool_type = for_loop_range_pool<IndexType, Function>;
    job_dispatcher *m_dispatcher;
    for_loop_range_pool_type m_loop_pool;
    std::atomic<int> m_num_jobs;

    parallel_for_context(IndexType first, IndexType last, 
                         IndexType step, const Function *func,
                         job_dispatcher &dispatcher)
        : m_loop_pool(first, last, step, func)
        , m_dispatcher(&dispatcher)
        , m_num_jobs(0)
    {}
};

template<typename IndexType, typename Function>
void parallel_for_job_func(job::data_type &data) {
    auto archive = memory_input_archive(data.data(), data.size());
    intptr_t ctx_intptr;
    archive(ctx_intptr);
    auto *ctx = reinterpret_cast<parallel_for_context<IndexType, Function> *>(ctx_intptr);

    // Decrement job count and if zero delete `ctx` on exit.
    auto defer = std::shared_ptr<void>(nullptr, [ctx] (void *) { 
        auto num_jobs = ctx->m_num_jobs.fetch_sub(1, std::memory_order_relaxed) - 1;
        if (num_jobs == 0) delete ctx;
    });
    
    auto *loop = ctx->m_loop_pool.steal();
    if (!loop) {
        return;
    }

    // Dispatch child job.
    {
        ctx->m_num_jobs.fetch_add(1, std::memory_order_relaxed);

        auto child_job = job();
        child_job.data = data;
        child_job.func = &parallel_for_job_func<IndexType, Function>;
        ctx->m_dispatcher->async(child_job);
    }

    loop->run();
}

} // namespace detail

template<typename IndexType, typename Function>
void parallel_for(job_dispatcher &dispatcher, IndexType first, IndexType last, IndexType step, const Function &func) {
    EDYN_ASSERT(step > IndexType{0});

    // The last job to run will delete `ctx`.
    auto *ctx = new detail::parallel_for_context<IndexType, Function>(first, last, step, &func, dispatcher);
    
    // Get the first loop subrange which will run in this thread.
    auto *loop = ctx->m_loop_pool.steal();

    // Increment job count so the this execution unit is accounted for.
    ctx->m_num_jobs.fetch_add(1, std::memory_order_relaxed);

    // On exit decrement job count and if zero delete `ctx`.
    auto defer = std::shared_ptr<void>(nullptr, [ctx] (void *) { 
        auto num_jobs = ctx->m_num_jobs.fetch_sub(1, std::memory_order_relaxed) - 1;
        if (num_jobs == 0) delete ctx;
    });

    // Create child job which will steal a range of the for-loop if it gets a
    // chance to be executed.
    {
        ctx->m_num_jobs.fetch_add(1, std::memory_order_relaxed);

        auto child_job = job();
        child_job.func = &detail::parallel_for_job_func<IndexType, Function>;
        auto archive = fixed_memory_output_archive(child_job.data.data(), child_job.data.size());
        auto ctx_intptr = reinterpret_cast<intptr_t>(ctx);
        archive(ctx_intptr);
        dispatcher.async(child_job);
    }

    loop->run();

    // Wait for all for-loops to complete.
    ctx->m_loop_pool.wait();
}

template<typename IndexType, typename Function>
void parallel_for(IndexType first, IndexType last, IndexType step, const Function &func) {
    parallel_for(job_dispatcher::global(), first, last, step, func);
}

template<typename IndexType, typename Function>
void parallel_for(IndexType first, IndexType last, const Function &func) {
    parallel_for(first, last, IndexType {1}, func);
}

template<typename Iterator, typename Function>
void parallel_for_each(job_dispatcher &dispatcher, Iterator first, Iterator last, const Function &func) {
    auto count = std::distance(first, last);

    parallel_for(dispatcher, size_t{0}, static_cast<size_t>(count), size_t{1}, [&] (size_t index) {
        func(first + index);
    });
}

template<typename Iterator, typename Function>
void parallel_for_each(Iterator first, Iterator last, const Function &func) {
    parallel_for_each(job_dispatcher::global(), first, last, func);
}

}

#endif // EDYN_PARALLEL_PARALLEL_FOR_HPP