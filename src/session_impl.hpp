#pragma once

#include "nghttp2-corosio/session.hpp"

#include <boost/capy/ex/async_event.hpp>
#include <boost/capy/io/any_stream.hpp>
#include <boost/capy/io_task.hpp>
#include <boost/capy/task.hpp>

#include <memory>

struct nghttp2_session;

namespace nghttp2_corosio
{

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
   /// send_loop()/recv_loop() until the connection closes. Doesn't dispatch requests yet: the
   /// nghttp2 callbacks registered here are still dummies.
   boost::capy::task<> run();

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

   /// Wakes a send_loop() suspended in write_ready_.wait(). Harmless to call when send_loop() is
   /// not currently waiting.
   void start_write() { write_ready_.set(); }

   boost::capy::any_executor executor_;
   boost::capy::any_stream stream_;
   Role role_;

   nghttp2_session* session_ = nullptr;
   boost::capy::async_event write_ready_;
};

// =================================================================================================

} // namespace nghttp2_corosio
