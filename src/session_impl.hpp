#pragma once

#include "nghttp2-corosio/server.hpp"
#include "nghttp2-corosio/session.hpp"

#include <boost/capy/ex/async_event.hpp>
#include <boost/capy/io/any_stream.hpp>
#include <boost/capy/io_task.hpp>
#include <boost/capy/task.hpp>

#include <cstdint>
#include <memory>
#include <string_view>
#include <unordered_map>

struct nghttp2_session;

namespace nghttp2_corosio
{

class Stream;

// =================================================================================================

class Session::Impl : public std::enable_shared_from_this<Session::Impl>
{
public:
   /// Both roles are driven by the exact same send_loop()/recv_loop(); the only difference is
   /// which nghttp2_session_*_new2() run() calls to set up the underlying nghttp2_session. Unlike
   /// anyhttp (which templates NGHttp2SessionImpl<Stream> on the stream type to share this code
   /// across server/client), we don't need that here: stream_ is already type-erased to
   /// any_stream, so send_loop()/recv_loop() are shared as ordinary member functions regardless
   /// of which stream implementation is behind them.
   enum class Role
   {
      server,
      client
   };

   /// `handler` is only meaningful for a server-role session (invoked per request, see
   /// handle_request()); pass a default-constructed one for a client-role session.
   Impl(boost::capy::any_executor executor, boost::capy::any_stream stream, Role role,
        RequestHandler handler)
      : executor_(std::move(executor)), stream_(std::move(stream)), role_(role),
        handler_(std::move(handler))
   {
   }
   ~Impl();

   boost::capy::any_executor get_executor() const noexcept { return executor_; }

   /// Sets up the nghttp2 session (callbacks, options, initial SETTINGS), then drives it via
   /// send_loop()/recv_loop() until the connection closes. On a server session, every incoming
   /// request's response is submitted (fixed 200 status) and then handed to `handler_`, if set --
   /// see handle_request().
   boost::capy::task<> run();

   /// Underlying nghttp2 session, valid once run() has set it up. Used by Stream to drive
   /// per-stream flow control (nghttp2_session_consume_stream()) and to resume a deferred data
   /// provider (nghttp2_session_resume_data()).
   nghttp2_session* native_handle() const noexcept { return session_; }

   /// Wakes a send_loop() suspended in write_ready_.wait(). Harmless to call when send_loop() is
   /// not currently waiting. Public because Stream also calls this after handing send_loop() new
   /// work (a submitted request/response, or bytes consumed that may reopen flow control).
   void start_write() { write_ready_.set(); }

   /// Submits a request on a new stream (client sessions only). See Session::submit_request().
   boost::capy::io_task<Session::Writer, Session::ClientResponse>
   submit_request(std::string_view path);

   std::shared_ptr<Stream> create_stream(std::int32_t id);
   std::shared_ptr<Stream> find_stream(std::int32_t id) const;
   void close_stream(std::int32_t id);

   /// Spawns (detached) handle_request() for a newly-arrived request.
   void dispatch_request(std::shared_ptr<Stream> stream);

   /// Submits the response HEADERS frame (`:status` plus `headers`) for `stream`, wiring up its
   /// data provider. Called lazily by Session::Response -- see handle_request().
   boost::capy::io_task<> submit_response(std::shared_ptr<Stream> stream, unsigned int status,
                                          const Session::Headers& headers);

private:
   /// Repeatedly pulls pending output out of nghttp2 (nghttp2_session_mem_send2) and writes it to
   /// the stream, coalescing small chunks into fewer writes. Suspends on `write_ready_` once
   /// nghttp2 has nothing left to send but still wants to read or write; ends once nghttp2 wants
   /// neither.
   ///
   /// The final start_write() from recv_loop() (waking this up to flush the closing GOAWAY after
   /// the peer disconnects) has been observed to occasionally go unanswered, apparently a corosio
   /// scheduler edge case around same-thread wakeups posted right before the scheduler would
   /// otherwise go idle -- reproduced with nothing nghttp2-specific involved. If that happens, this
   /// coroutine is left suspended here indefinitely (the session goes idle rather than closing
   /// promptly) until something cancels it -- Server::~Server()'s structured shutdown does exactly
   /// that (see detail::TaskGroup in task_group.hpp), which is also what makes it safe to destroy a
   /// Server regardless of whether any session has fully wound down.
   boost::capy::io_task<> send_loop();

   /// Reads bytes from the stream and feeds them to nghttp2 (nghttp2_session_mem_recv2), which
   /// synchronously invokes whatever callbacks are registered. Wakes send_loop() after every
   /// chunk, since parsing new input can produce new output (e.g. SETTINGS acks). Ends once the
   /// stream errors or nghttp2 wants neither to read nor write.
   boost::capy::io_task<> recv_loop();

   /// Builds a Request/Response pair for a newly-arrived request and hands them to `handler_` (if
   /// set; otherwise the response is just closed empty). The response isn't submitted here -- see
   /// Session::Response -- so the handler can set a status/headers before its first write.
   /// Spawned (detached) per request by the HEADERS/REQUEST case in on_frame_recv_callback().
   boost::capy::task<> handle_request(std::shared_ptr<Stream> stream);

   boost::capy::any_executor executor_;
   boost::capy::any_stream stream_;
   Role role_;
   RequestHandler handler_;

   nghttp2_session* session_ = nullptr;
   boost::capy::async_event write_ready_;
   std::unordered_map<std::int32_t, std::shared_ptr<Stream>> streams_;
};

// =================================================================================================

} // namespace nghttp2_corosio
