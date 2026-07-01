#include "nghttp2-corosio/session.hpp"
#include "session_impl.hpp"

#include <boost/capy/buffers.hpp>
#include <boost/capy/when_all.hpp>
#include <boost/capy/write.hpp>

#include <nghttp2/nghttp2.h>

#include <array>
#include <memory>
#include <print>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace nghttp2_corosio
{

// =================================================================================================

namespace
{

std::string_view frame_type_name(std::uint8_t type)
{
   switch (type)
   {
   case NGHTTP2_DATA:
      return "DATA";
   case NGHTTP2_HEADERS:
      return "HEADERS";
   case NGHTTP2_PRIORITY:
      return "PRIORITY";
   case NGHTTP2_RST_STREAM:
      return "RST_STREAM";
   case NGHTTP2_SETTINGS:
      return "SETTINGS";
   case NGHTTP2_PUSH_PROMISE:
      return "PUSH_PROMISE";
   case NGHTTP2_PING:
      return "PING";
   case NGHTTP2_GOAWAY:
      return "GOAWAY";
   case NGHTTP2_WINDOW_UPDATE:
      return "WINDOW_UPDATE";
   case NGHTTP2_CONTINUATION:
      return "CONTINUATION";
   case NGHTTP2_ALTSVC:
      return "ALTSVC";
   case NGHTTP2_ORIGIN:
      return "ORIGIN";
   default:
      return "UNKNOWN";
   }
}

std::string_view to_string_view(const uint8_t* data, std::size_t len)
{
   return {reinterpret_cast<const char*>(data), len};
}

std::string_view to_string_view(nghttp2_rcbuf* buf)
{
   auto v = nghttp2_rcbuf_get_buf(buf);
   return to_string_view(v.base, v.len);
}

// -------------------------------------------------------------------------------------------------
// nghttp2 callbacks
//
// None of these do any request handling yet -- that requires per-stream state (an equivalent of
// anyhttp's NGHttp2Stream) that doesn't exist in this codebase yet. For now they just log what's
// happening and let nghttp2 parse the connection according to the HTTP/2 protocol (SETTINGS, PING,
// WINDOW_UPDATE, etc. are all handled internally by nghttp2 regardless of what callbacks are
// registered).
// -------------------------------------------------------------------------------------------------

int on_begin_headers_callback(nghttp2_session*, const nghttp2_frame* frame, void*)
{
   std::println("[{}] on_begin_headers_callback", frame->hd.stream_id);
   return 0;
}

int on_header_callback(nghttp2_session*, const nghttp2_frame* frame, const uint8_t* name,
                        std::size_t namelen, const uint8_t* value, std::size_t valuelen,
                        std::uint8_t, void*)
{
   std::println("[{}] {}: {}", frame->hd.stream_id, to_string_view(name, namelen),
                to_string_view(value, valuelen));
   return 0;
}

int on_frame_not_send_callback(nghttp2_session*, const nghttp2_frame* frame, int lib_error_code,
                                void*)
{
   std::println("[{}] on_frame_not_send_callback: {} {}", frame->hd.stream_id,
                frame_type_name(frame->hd.type), nghttp2_strerror(lib_error_code));
   return 0;
}

int on_error_callback(nghttp2_session*, int, const char* msg, std::size_t len, void*)
{
   std::println("nghttp2 error: {}", std::string_view(msg, len));
   return 0;
}

int on_invalid_header_callback(nghttp2_session*, const nghttp2_frame* frame, nghttp2_rcbuf* name,
                                nghttp2_rcbuf* value, std::uint8_t, void*)
{
   std::println("[{}] on_invalid_header_callback: {}: {}", frame->hd.stream_id, to_string_view(name),
                to_string_view(value));
   return 0;
}

int on_frame_recv_callback(nghttp2_session*, const nghttp2_frame* frame, void*)
{
   std::println("[{}] on_frame_recv_callback: {} length={} flags={}", frame->hd.stream_id,
                frame_type_name(frame->hd.type), frame->hd.length, frame->hd.flags);
   return 0;
}

int on_data_chunk_recv_callback(nghttp2_session*, std::uint8_t, std::int32_t stream_id,
                                 const uint8_t*, std::size_t len, void*)
{
   std::println("[{}] on_data_chunk_recv_callback: {} bytes", stream_id, len);
   return 0;
}

int on_frame_send_callback(nghttp2_session*, const nghttp2_frame* frame, void*)
{
   std::println("[{}] on_frame_send_callback: {} length={} flags={}", frame->hd.stream_id,
                frame_type_name(frame->hd.type), frame->hd.length, frame->hd.flags);
   return 0;
}

int on_stream_close_callback(nghttp2_session* session, std::int32_t stream_id,
                              std::uint32_t error_code, void*)
{
   bool local_close = nghttp2_session_get_stream_local_close(session, stream_id);
   bool remote_close = nghttp2_session_get_stream_remote_close(session, stream_id);
   std::println("[{}] on_stream_close_callback: {} (local={}, remote={})", stream_id,
                nghttp2_http2_strerror(error_code), local_close, remote_close);
   return 0;
}

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

   std::println("session created");

   nghttp2_settings_entry ent{NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 100};
   nghttp2_submit_settings(session_, NGHTTP2_FLAG_NONE, &ent, 1);

   [[maybe_unused]] auto result = co_await boost::capy::when_all(send_loop(), recv_loop());

   std::println("session ended");
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
         std::println("send loop: nghttp2_session_mem_send2 failed: {}",
                      nghttp2_strerror(static_cast<int>(nread)));
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
         std::println("send loop: writing {} bytes...", bytes_to_write);
         std::array<boost::capy::const_buffer, 2> seq{
            boost::capy::const_buffer(pending.data(), pending.size()),
            boost::capy::const_buffer(data, static_cast<std::size_t>(nread))};
         auto [ec, written] = co_await boost::capy::write(stream_, seq);
         pending.clear();
         if (ec)
         {
            std::println("send loop: write error: {}", ec.message());
            break;
         }
         continue;
      }

      // Nothing buffered, nothing new: either we're done, or we wait to be poked by recv_loop()
      // (via start_write()) once it has fed nghttp2 more input.
      if (!nghttp2_session_want_read(session_) && !nghttp2_session_want_write(session_))
         break;

      std::println("send loop: waiting...");
      write_ready_.clear();
      if (auto [ec] = co_await write_ready_.wait(); ec)
         break;
   }

   std::println("send loop: done");
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
         std::println("recv loop: {}, terminating session", ec.message());
         break;
      }

      std::println("recv loop: read {} bytes", n);
      if (auto rv = nghttp2_session_mem_recv2(session_, buffer.data(), n); rv < 0)
      {
         std::println("recv loop: nghttp2_session_mem_recv2 failed: {}",
                      nghttp2_strerror(static_cast<int>(rv)));
         nghttp2_session_terminate_session(session_, NGHTTP2_PROTOCOL_ERROR);
         start_write();
         break;
      }

      // Parsing the new input may have produced output to send (e.g. a SETTINGS ack).
      start_write();
   }

   nghttp2_session_terminate_session(session_, NGHTTP2_NO_ERROR);
   start_write(); // wake send_loop() so it can flush the GOAWAY and notice it's done

   std::println("recv loop: done");
   co_return boost::capy::io_result<>{};
}

// =================================================================================================

} // namespace nghttp2_corosio
