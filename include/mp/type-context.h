// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef MP_PROXY_TYPE_CONTEXT_H
#define MP_PROXY_TYPE_CONTEXT_H

#include <mp/proxy-io.h>
#include <mp/util.h>

#include <atomic>

namespace mp {

//! CustomBuildField for mp.Context. Runs on the calling thread before an IPC
//! request is sent. Assigns a stable clientThreadId if this thread doesn't have
//! one yet, creates a WorkerThread callback proxy for this thread (so incoming
//! callbacks can be posted to its Waiter), and writes the ID into the Context
//! field.
//!
//! If the calling thread is a server-side pool thread currently handling a
//! request, propagates that request's clientThreadId so the callback routes
//! back to the correct application thread on the remote side.
template <typename Output>
void CustomBuildField(TypeList<>,
    Priority<1>,
    ClientInvokeContext& invoke_context,
    Output&& output,
    typename std::enable_if<std::is_same<decltype(output.get()), Context::Builder>::value>::type* enable = nullptr)
{
    auto& connection = invoke_context.connection;
    auto& thread_context = invoke_context.thread_context;

    // Server pool thread making a callback: propagate the clientThreadId from
    // the request currently being served so the callback is routed back to the
    // originating application thread on the other side.
    if (thread_context.waiter) {
        Lock lock(thread_context.waiter->m_mutex);
        auto it = thread_context.serving_client_ids.find(&connection);
        if (it != thread_context.serving_client_ids.end()) {
            auto context = output.init();
            context.setClientThreadId(it->second);
            return;
        }
    }

    // Application client thread: assign a stable numeric ID on first use.
    if (thread_context.thread_id == 0) {
        thread_context.thread_id = NextClientThreadId();
    }

    // Register a WorkerThread callback proxy for this thread if not already
    // present, so PassField on the receiving side can post callbacks to us.
    connection.m_loop->sync([&] {
        if (connection.m_thread_assignments.find(thread_context.thread_id) == connection.m_thread_assignments.end()) {
            // Client-side WorkerThread: no OS thread (std::thread{}), just
            // wraps this thread's Waiter so incoming callbacks can be posted.
            auto worker = kj::refcounted<WorkerThread>(connection, thread_context, std::thread{});
            connection.m_thread_assignments[thread_context.thread_id] = worker.get();
            connection.m_thread_pool.push_back(std::move(worker));
        }
    });

    auto context = output.init();
    context.setClientThreadId(thread_context.thread_id);
}

//! PassField override for mp.Context arguments. Unified for both directions:
//! - Server side (C→S): look up or create a pool WorkerThread for clientThreadId,
//!   then post the invocation to it.
//! - Client side (S→C callbacks): look up the callback proxy WorkerThread
//!   registered by CustomBuildField, then post to that thread's Waiter.
template <typename Accessor, typename ServerContext, typename Fn, typename... Args>
auto PassField(Priority<1>, TypeList<>, ServerContext& server_context, const Fn& fn, Args&&... args) ->
    typename std::enable_if<
        std::is_same<decltype(Accessor::get(server_context.call_context.getParams())), Context::Reader>::value,
        kj::Promise<typename ServerContext::CallContext>>::type
{
    auto& server = server_context.proxy_server;
    EventLoop& loop = *server.m_context.loop;
    Connection& connection = *server.m_context.connection;
    int req = server_context.req;
    auto self = server.thisCap();

    const auto& params = server_context.call_context.getParams();
    uint64_t client_thread_id = Accessor::get(params).getClientThreadId();

    // Look up the WorkerThread for this clientThreadId, or create one (server
    // side only — client side always pre-registers in CustomBuildField).
    WorkerThread* thread_ptr = nullptr;
    loop.sync([&] {
        auto it = connection.m_thread_assignments.find(client_thread_id);
        if (it != connection.m_thread_assignments.end()) {
            thread_ptr = it->second;
        } else {
            thread_ptr = &MakeWorkerThread(connection, client_thread_id);
        }
    });

    auto invoke = [self = kj::mv(self), call_context = kj::mv(server_context.call_context), &server, &loop, &connection, req, client_thread_id, fn, args...](CancelMonitor& cancel_monitor) mutable {
        MP_LOG(loop, Log::Debug) << "IPC server executing request #" << req;
        if (loop.testing_hook_async_request_start) loop.testing_hook_async_request_start();
        KJ_DEFER(if (loop.testing_hook_async_request_done) loop.testing_hook_async_request_done());
        ServerContext server_context{server, call_context, req};

        // Record which clientThreadId we are serving so that any outgoing IPC
        // calls made from this worker thread (callbacks) propagate the same ID,
        // keeping the call chain routed to the correct thread on both sides.
        auto& thread_context = g_thread_context;
        {
            Lock lock(thread_context.waiter->m_mutex);
            thread_context.serving_client_ids[&connection] = client_thread_id;
        }
        KJ_DEFER({
            Lock lock(thread_context.waiter->m_mutex);
            thread_context.serving_client_ids.erase(&connection);
        });

        Mutex cancel_mutex;
        Lock cancel_lock{cancel_mutex};
        server_context.cancel_lock = &cancel_lock;

        loop.sync([&] {
            if (cancel_monitor.m_canceled) {
                server_context.request_canceled = true;
                return;
            }
            assert(!cancel_monitor.m_on_cancel);
            cancel_monitor.m_on_cancel = [&loop, &server_context, &cancel_mutex, req]() {
                MP_LOG(loop, Log::Info) << "IPC server request #" << req << " canceled while executing.";
                Lock cancel_lock{cancel_mutex};
                server_context.request_canceled = true;
            };
        });

        KJ_DEFER(
            cancel_lock.m_lock.unlock();
            loop.sync([&] {
                cancel_monitor.m_on_cancel = nullptr;
                auto self_dispose{kj::mv(self)};
            });
        );

        if (server_context.request_canceled) {
            MP_LOG(loop, Log::Info) << "IPC server request #" << req << " canceled before it could be executed";
        } else KJ_IF_MAYBE(exception, kj::runCatchingExceptions([&]{
            try {
                fn.invoke(server_context, args...);
            } catch (const InterruptException& e) {
                MP_LOG(loop, Log::Info) << "IPC server request #" << req << " interrupted (" << e.what() << ")";
            }
        })) {
            MP_LOG(loop, Log::Error) << "IPC server request #" << req << " uncaught exception (" << kj::str(*exception).cStr() << ")";
            kj::throwRecoverableException(kj::mv(*exception));
        }
        return call_context;
    };

    auto result = thread_ptr->post<typename ServerContext::CallContext>(std::move(invoke));
    return connection.m_canceler.wrap(kj::mv(result));
}
} // namespace mp

#endif // MP_PROXY_TYPE_CONTEXT_H
