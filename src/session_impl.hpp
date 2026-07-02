#pragma once

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

   Impl(boost::capy::any_executor executor, boost::capy::any_stream stream, Role role)
      : executor_(std::move(executor)), stream_(std::move(stream)), role_(role)
   {
   }
   ~Impl();

   boost::capy::any_executor get_executor() const noexcept { return executor_; }

   /// Sets up the nghttp2 session (callbacks, options, initial SETTINGS), then drives it via
   /// send_loop()/recv_loop() until the connection closes. On a server session, every incoming
   /// request is handled by a hardcoded echo handler (see handle_request()) -- a pluggable
   /// request-handler API will come later.
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
   boost::capy::io_task<Session::Writer, Session::Reader> submit_request(std::string_view path);

   std::shared_ptr<Stream> create_stream(std::int32_t id);
   std::shared_ptr<Stream> find_stream(std::int32_t id) const;
   void close_stream(std::int32_t id);

   /// Spawns (detached) the hardcoded echo handler for a newly-arrived request.
   void dispatch_request(std::shared_ptr<Stream> stream);

private:
   /// Repeatedly pulls pending output out of nghttp2 (nghttp2_session_mem_send2) and writes it to
   /// the stream, coalescing small chunks into fewer writes. Suspends on `write_ready_` once
   /// nghttp2 has nothing left to send but still wants to read or write; ends once nghttp2 wants
   /// neither.
   ///
   /// Known issue: the final start_write() from recv_loop() (waking this up to flush the closing
   /// GOAWAY after the peer disconnects) is occasionally missed, apparently a corosio scheduler
   /// edge case around same-thread wakeups posted right before the scheduler would otherwise go
   /// idle -- reproduced with nothing nghttp2-specific involved. When missed, this coroutine (and
   /// the socket it holds) leaks; see the caveat on Server::stop().
   boost::capy::io_task<> send_loop();

   /// Reads bytes from the stream and feeds them to nghttp2 (nghttp2_session_mem_recv2), which
   /// synchronously invokes whatever callbacks are registered. Wakes send_loop() after every
   /// chunk, since parsing new input can produce new output (e.g. SETTINGS acks). Ends once the
   /// stream errors or nghttp2 wants neither to read nor write.
   boost::capy::io_task<> recv_loop();

   /// Hardcoded server-side request handler: reads the request body to completion, then submits a
   /// 200 response echoing it back. Spawned (detached) per request by the HEADERS/REQUEST case in
   /// on_frame_recv_callback().
   boost::capy::task<> handle_request(std::shared_ptr<Stream> stream);

   boost::capy::any_executor executor_;
   boost::capy::any_stream stream_;
   Role role_;

   nghttp2_session* session_ = nullptr;
   boost::capy::async_event write_ready_;
   std::unordered_map<std::int32_t, std::shared_ptr<Stream>> streams_;
};

// =================================================================================================

} // namespace nghttp2_corosio
