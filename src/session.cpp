#include "nghttp2-corosio/session.hpp"
#include "session_impl.hpp"

#include <boost/capy/buffers.hpp>
#include <boost/capy/when_all.hpp>
#include <boost/capy/write.hpp>

#include <nghttp2/nghttp2.h>

#include <array>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

namespace nghttp2_corosio
{

// =================================================================================================

namespace
{

// -------------------------------------------------------------------------------------------------
// nghttp2 callbacks
//
// None of these do any request handling yet -- that requires per-stream state (an equivalent of
// anyhttp's NGHttp2Stream) that doesn't exist in this codebase yet. For now they just let nghttp2
// parse the connection according to the HTTP/2 protocol (SETTINGS, PING, WINDOW_UPDATE, etc. are
// all handled internally by nghttp2 regardless of what callbacks are registered).
// -------------------------------------------------------------------------------------------------

int on_begin_headers_callback(nghttp2_session*, const nghttp2_frame*, void*) { return 0; }

int on_header_callback(nghttp2_session*, const nghttp2_frame*, const uint8_t*, std::size_t,
                        const uint8_t*, std::size_t, std::uint8_t, void*)
{
   return 0;
}

int on_frame_not_send_callback(nghttp2_session*, const nghttp2_frame* frame, int lib_error_code,
                                void*)
{
   std::cerr << "nghttp2: frame not sent (type=" << static_cast<int>(frame->hd.type)
             << "): " << nghttp2_strerror(lib_error_code) << "\n";
   return 0;
}

int on_error_callback(nghttp2_session*, int, const char* msg, std::size_t len, void*)
{
   std::cerr << "nghttp2 error: " << std::string_view(msg, len) << "\n";
   return 0;
}

int on_invalid_header_callback(nghttp2_session*, const nghttp2_frame*, nghttp2_rcbuf*,
                                nghttp2_rcbuf*, std::uint8_t, void*)
{
   return 0;
}

int on_frame_recv_callback(nghttp2_session*, const nghttp2_frame*, void*) { return 0; }

int on_data_chunk_recv_callback(nghttp2_session*, std::uint8_t, std::int32_t, const uint8_t*,
                                 std::size_t, void*)
{
   return 0;
}

int on_frame_send_callback(nghttp2_session*, const nghttp2_frame*, void*) { return 0; }

int on_stream_close_callback(nghttp2_session*, std::int32_t, std::uint32_t, void*) { return 0; }

using session_callbacks_ptr =
   std::unique_ptr<nghttp2_session_callbacks, void (*)(nghttp2_session_callbacks*)>;

session_callbacks_ptr setup_callbacks()
{
   nghttp2_session_callbacks* raw = nullptr;
   if (nghttp2_session_callbacks_new(&raw))
      throw std::runtime_error("nghttp2_session_callbacks_new failed");
   session_callbacks_ptr callbacks(raw, &nghttp2_session_callbacks_del);

   // clang-format off
   nghttp2_session_callbacks_set_on_begin_headers_callback  (callbacks.get(), on_begin_headers_callback);
   nghttp2_session_callbacks_set_on_header_callback         (callbacks.get(), on_header_callback);
   nghttp2_session_callbacks_set_on_frame_not_send_callback (callbacks.get(), on_frame_not_send_callback);
   nghttp2_session_callbacks_set_error_callback2            (callbacks.get(), on_error_callback);
   nghttp2_session_callbacks_set_on_invalid_header_callback2(callbacks.get(), on_invalid_header_callback);
   nghttp2_session_callbacks_set_on_frame_recv_callback     (callbacks.get(), on_frame_recv_callback);
   nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks.get(), on_data_chunk_recv_callback);
   nghttp2_session_callbacks_set_on_frame_send_callback     (callbacks.get(), on_frame_send_callback);
   nghttp2_session_callbacks_set_on_stream_close_callback   (callbacks.get(), on_stream_close_callback);
   // clang-format on

   return callbacks;
}

} // namespace

// =================================================================================================

Session::Session(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}
Session::Session(Session&&) noexcept = default;
Session& Session::operator=(Session&&) noexcept = default;
Session::~Session() = default;

Session::executor_type Session::get_executor() const noexcept { return impl_->get_executor(); }

// =================================================================================================

Session::Impl::~Impl()
{
   if (session_)
      nghttp2_session_del(session_);
}

// -------------------------------------------------------------------------------------------------

boost::capy::task<> Session::Impl::run()
{
   auto self = shared_from_this(); // keep the session alive for the duration of the coroutine

   nghttp2_option* raw_options = nullptr;
   if (nghttp2_option_new(&raw_options))
      throw std::runtime_error("nghttp2_option_new failed");
   std::unique_ptr<nghttp2_option, void (*)(nghttp2_option*)> options(raw_options,
                                                                       &nghttp2_option_del);

   // We'll be submitting WINDOW_UPDATE ourselves once flow control is implemented, so disable
   // nghttp2's automatic ones now to avoid the two fighting later. See nghttp2/nghttp2#446.
   nghttp2_option_set_no_auto_window_update(options.get(), 1);

   auto callbacks = setup_callbacks();
   if (nghttp2_session_server_new2(&session_, callbacks.get(), this, options.get()))
      throw std::runtime_error("nghttp2_session_server_new2 failed");

   nghttp2_settings_entry ent{NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 100};
   nghttp2_submit_settings(session_, NGHTTP2_FLAG_NONE, &ent, 1);

   [[maybe_unused]] auto result = co_await boost::capy::when_all(send_loop(), recv_loop());

   std::cout << "Session ended\n";
}

// -------------------------------------------------------------------------------------------------

boost::capy::io_task<> Session::Impl::send_loop()
{
   // Accumulates small chunks returned by nghttp2_session_mem_send2() so we don't issue a
   // separate stream write for each one.
   std::vector<std::uint8_t> pending;
   pending.reserve(1460);

   for (;;)
   {
      const std::uint8_t* data = nullptr;
      auto nread = nghttp2_session_mem_send2(session_, &data);
      if (nread < 0)
      {
         std::cerr << "send loop: nghttp2_session_mem_send2 failed: "
                    << nghttp2_strerror(static_cast<int>(nread)) << "\n";
         break;
      }

      // If the new chunk fits into what's left of the accumulator, buffer it and go ask nghttp2
      // for more instead of writing right away.
      if (nread > 0 && static_cast<std::size_t>(nread) <= pending.capacity() - pending.size())
      {
         pending.insert(pending.end(), data, data + nread);
         continue;
      }

      // Otherwise, if there is anything to send -- whether buffered, freshly returned, or both --
      // write it all out in one go and go back to ask nghttp2 for more.
      if (const auto bytes_to_write = pending.size() + static_cast<std::size_t>(nread);
          bytes_to_write > 0)
      {
         std::array<boost::capy::const_buffer, 2> seq{
            boost::capy::const_buffer(pending.data(), pending.size()),
            boost::capy::const_buffer(data, static_cast<std::size_t>(nread))};
         auto [ec, written] = co_await boost::capy::write(stream_, seq);
         pending.clear();
         if (ec)
         {
            std::cerr << "send loop: write error: " << ec.message() << "\n";
            break;
         }
         continue;
      }

      // Nothing buffered, nothing new: either we're done, or we wait to be poked by recv_loop()
      // (via start_write()) once it has fed nghttp2 more input.
      if (!nghttp2_session_want_read(session_) && !nghttp2_session_want_write(session_))
         break;

      write_ready_.clear();
      if (auto [ec] = co_await write_ready_.wait(); ec)
         break;
   }

   co_return boost::capy::io_result<>{};
}

// -------------------------------------------------------------------------------------------------

boost::capy::io_task<> Session::Impl::recv_loop()
{
   std::vector<std::uint8_t> buffer(64 * 1024);

   while (nghttp2_session_want_read(session_) || nghttp2_session_want_write(session_))
   {
      auto [ec, n] =
         co_await stream_.read_some(boost::capy::mutable_buffer(buffer.data(), buffer.size()));
      if (ec)
      {
         std::cout << "recv loop: " << ec.message() << ", terminating session\n";
         break;
      }

      if (auto rv = nghttp2_session_mem_recv2(session_, buffer.data(), n); rv < 0)
      {
         std::cerr << "recv loop: nghttp2_session_mem_recv2 failed: "
                    << nghttp2_strerror(static_cast<int>(rv)) << "\n";
         nghttp2_session_terminate_session(session_, NGHTTP2_PROTOCOL_ERROR);
         start_write();
         break;
      }

      // Parsing the new input may have produced output to send (e.g. a SETTINGS ack).
      start_write();
   }

   nghttp2_session_terminate_session(session_, NGHTTP2_NO_ERROR);
   start_write(); // wake send_loop() so it can flush the GOAWAY and notice it's done

   co_return boost::capy::io_result<>{};
}

// =================================================================================================

} // namespace nghttp2_corosio
