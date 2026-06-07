// Copyright (c) 2026 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef MP_THREAD_POOL_H
#define MP_THREAD_POOL_H

#include <mp/util.h>

#include <kj/async.h>
#include <kj/common.h>
#include <kj/function.h>

#include <condition_variable>
#include <cstddef>
#include <queue>
#include <string>
#include <thread>
#include <vector>

namespace mp {

class EventLoop;

//! Fixed-size pool of worker threads sharing a single work queue.
//!
//! Tasks submitted via post() are pulled from the queue by the next idle
//! worker (work delegation), rather than being statically assigned to a
//! particular thread. This lets the pool absorb bursty or uneven workloads
//! without head-of-line blocking on a chosen worker.
//!
//! Worker bodies run on dedicated std::threads. The callable runs on the
//! worker; result fulfillment is marshaled back to the EventLoop thread
//! via EventLoop::sync() before touching kj promise state, since kj
//! promises are not thread-safe.
//!
//! Lifecycle:
//!   - construct, bound to an EventLoop
//!   - start(name, count) once, from the EventLoop thread
//!   - post() from any thread while the pool is running
//!   - stop() once, from a non-worker thread; the destructor calls it if
//!     the caller has not.
class ThreadPool
{
public:
    explicit ThreadPool(EventLoop& loop) : m_loop(loop) {}
    ~ThreadPool() { stop(); }

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&) = delete;
    ThreadPool& operator=(ThreadPool&&) = delete;

    //! Spawn `count` worker threads. May only be called once per pool.
    void start(const std::string& pool_name, std::size_t count);

    //! Signal all workers to exit once the queue drains, then join them.
    //! Idempotent. Must not be called from a worker thread.
    void stop();

    //! Submit a callable for execution on a worker thread. The returned
    //! promise resolves on the EventLoop thread with the value fn()
    //! returned, or is rejected if fn() threw.
    //!
    //! The template body is defined in proxy-io.h, after EventLoop is
    //! fully visible. Include <mp/proxy-io.h> before calling post().
    template <typename T, typename Fn>
    kj::Promise<T> post(Fn&& fn);

    //! Number of worker threads. Stable between start() and stop().
    std::size_t size() const { return m_size; }

private:
    void workerLoop(const std::string& worker_name);

    EventLoop& m_loop;
    Mutex m_mutex;
    std::condition_variable m_cv;
    std::queue<kj::Function<void()>> m_queue MP_GUARDED_BY(m_mutex);
    bool m_started MP_GUARDED_BY(m_mutex){false};
    bool m_stopped MP_GUARDED_BY(m_mutex){false};
    std::vector<std::thread> m_workers;
    std::size_t m_size{0};
};

} // namespace mp

#endif // MP_THREAD_POOL_H
