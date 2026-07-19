#include "nghttp2-corosio/session.hpp"
#include "nghttp2-corosio/logging.hpp"
#include "session_impl.hpp"
#include "stream_impl.hpp"
#include "task_group.hpp"

#include <boost/capy/buffers.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/read.hpp>
#include <boost/capy/when_all.hpp>
#include <boost/capy/write.hpp>

#include <nghttp2/nghttp2.h>

#include <array>
#include <charconv>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
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

nghttp2_nv make_nv(std::string_view name, std::string_view value)
{
   return {reinterpret_cast<uint8_t*>(const_cast<char*>(name.data())),
           reinterpret_cast<uint8_t*>(const_cast<char*>(value.data())), name.size(), value.size(),
           NGHTTP2_NV_FLAG_NONE};
}

/// Trampoline nghttp2 uses to pull outgoing body bytes; `source->ptr` is the Stream set up by
/// handle_request()/Session::Impl::submit_request().
nghttp2_ssize data_source_read_callback(nghttp2_session*, std::int32_t, std::uint8_t* buf,
                                        std::size_t length, std::uint32_t* data_flags,
                                        nghttp2_data_source* source, void*)
{
   auto* stream = static_cast<Stream*>(source->ptr);
   return static_cast<nghttp2_ssize>(stream->producer_callback(buf, length, data_flags));
}

// -------------------------------------------------------------------------------------------------
// nghttp2 callbacks
//
// These wire nghttp2's protocol-level events into the per-stream Stream objects (src/stream.cpp),
// which bridge them to the coroutine-native StreamReader/StreamWriter. Only the :path
// pseudo-header is captured (Stream::path(), surfaced via Session::Request::path()) -- there's no
// general headers API yet.
// -------------------------------------------------------------------------------------------------

int on_begin_headers_callback(nghttp2_session*, const nghttp2_frame* frame, void* user_data)
{
   logd("[{}] on_begin_headers_callback", frame->hd.stream_id);

   // Only server sessions create a stream here: for a client session's response headers, the
   // stream already exists (created by submit_request() before the request was even sent).
   if (frame->hd.type == NGHTTP2_HEADERS && frame->headers.cat == NGHTTP2_HCAT_REQUEST)
      static_cast<Session::Impl*>(user_data)->create_stream(frame->hd.stream_id);

   return 0;
}

int on_header_callback(nghttp2_session*, const nghttp2_frame* frame, const uint8_t* name,
                       std::size_t namelen, const uint8_t* value, std::size_t valuelen,
                       std::uint8_t, void* user_data)
{
   auto name_sv = to_string_view(name, namelen);
   auto value_sv = to_string_view(value, valuelen);
   logd("[{}] {}: {}", frame->hd.stream_id, name_sv, value_sv);

   if (name_sv == ":path")
   {
      if (auto stream = static_cast<Session::Impl*>(user_data)->find_stream(frame->hd.stream_id))
         stream->set_path(std::string(value_sv));
   }
   else if (name_sv == ":status")
   {
      unsigned int status = 0;
      std::from_chars(value_sv.data(), value_sv.data() + value_sv.size(), status);
      if (auto stream = static_cast<Session::Impl*>(user_data)->find_stream(frame->hd.stream_id))
         stream->set_status(status);
   }

   return 0;
}

int on_frame_not_send_callback(nghttp2_session*, const nghttp2_frame* frame, int lib_error_code,
                               void*)
{
   logw("[{}] on_frame_not_send_callback: {} {}", frame->hd.stream_id,
        frame_type_name(frame->hd.type), nghttp2_strerror(lib_error_code));
   return 0;
}

int on_error_callback(nghttp2_session*, int, const char* msg, std::size_t len, void*)
{
   loge("nghttp2 error: {}", std::string_view(msg, len));
   return 0;
}

int on_invalid_header_callback(nghttp2_session*, const nghttp2_frame* frame, nghttp2_rcbuf* name,
                               nghttp2_rcbuf* value, std::uint8_t, void*)
{
   logw("[{}] on_invalid_header_callback: {}: {}", frame->hd.stream_id, to_string_view(name),
        to_string_view(value));
   return 0;
}

int on_frame_recv_callback(nghttp2_session*, const nghttp2_frame* frame, void* user_data)
{
   logd("[{}] on_frame_recv_callback: {} length={} flags={}", frame->hd.stream_id,
        frame_type_name(frame->hd.type), frame->hd.length, frame->hd.flags);

   auto* session = static_cast<Session::Impl*>(user_data);

   // Full request headers received: dispatch the (currently hardcoded) request handler.
   if (frame->hd.type == NGHTTP2_HEADERS && frame->headers.cat == NGHTTP2_HCAT_REQUEST)
      if (auto stream = session->find_stream(frame->hd.stream_id))
         session->dispatch_request(std::move(stream));

   // HEADERS or DATA carrying END_STREAM: the peer is done sending on this stream.
   if ((frame->hd.type == NGHTTP2_HEADERS || frame->hd.type == NGHTTP2_DATA) &&
       (frame->hd.flags & NGHTTP2_FLAG_END_STREAM))
      if (auto stream = session->find_stream(frame->hd.stream_id))
         stream->on_read_eof();

   return 0;
}

int on_data_chunk_recv_callback(nghttp2_session* raw_session, std::uint8_t, std::int32_t stream_id,
                                const uint8_t* data, std::size_t len, void* user_data)
{
   logd("[{}] on_data_chunk_recv_callback: {} bytes", stream_id, len);

   // Connection-level flow control is replenished right away, regardless of whether the user has
   // read the bytes yet; per-stream flow control is replenished only once Stream::consume() is
   // called after the user actually reads them (see Stream::read_some()).
   nghttp2_session_consume_connection(raw_session, len);

   auto* session = static_cast<Session::Impl*>(user_data);
   if (auto stream = session->find_stream(stream_id))
      stream->on_data(data, len);

   return 0;
}

int on_frame_send_callback(nghttp2_session*, const nghttp2_frame* frame, void*)
{
   logd("[{}] on_frame_send_callback: {} length={} flags={}", frame->hd.stream_id,
        frame_type_name(frame->hd.type), frame->hd.length, frame->hd.flags);
   return 0;
}

int on_stream_close_callback(nghttp2_session* session, std::int32_t stream_id,
                             std::uint32_t error_code, void* user_data)
{
   bool local_close = nghttp2_session_get_stream_local_close(session, stream_id);
   bool remote_close = nghttp2_session_get_stream_remote_close(session, stream_id);
   logd("[{}] on_stream_close_callback: {} (local={}, remote={})", stream_id,
        nghttp2_http2_strerror(error_code), local_close, remote_close);

   static_cast<Session::Impl*>(user_data)->close_stream(stream_id);
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

boost::capy::io_task<Session::Writer, Session::ClientResponse>
Session::submit_request(std::string_view path)
{
   return impl_->submit_request(path);
}

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
   int rv = role_ == Role::server
               ? nghttp2_session_server_new2(&session_, callbacks.get(), this, options.get())
               : nghttp2_session_client_new2(&session_, callbacks.get(), this, options.get());
   if (rv)
      throw std::runtime_error(role_ == Role::server ? "nghttp2_session_server_new2 failed"
                                                     : "nghttp2_session_client_new2 failed");

   logi("session created ({})", role_ == Role::server ? "server" : "client");

   nghttp2_settings_entry ent{NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 100};
   nghttp2_submit_settings(session_, NGHTTP2_FLAG_NONE, &ent, 1);

   [[maybe_unused]] auto result = co_await boost::capy::when_all(send_loop(), recv_loop());

   // Abort any streams that never closed at the protocol level -- e.g. abandoned by the user
   // without writing/reading anything, or still in flight when the session ended (connection
   // error, cancellation). on_close() wakes anything still suspended on one (a Reader/Writer the
   // caller is still holding gets an eof/canceled error instead of hanging forever). Clearing
   // streams_ also breaks the Stream <-> Session::Impl reference cycle (streams_ holds a
   // shared_ptr<Stream>, Stream holds a shared_ptr<Session::Impl> back to call native_handle()/
   // start_write()) that would otherwise keep both alive forever once nothing else references
   // either -- normally broken one stream at a time by close_stream(), driven by nghttp2's
   // on_stream_close_callback(), which never fires for a stream neither side ever closed.
   for (auto& [id, stream] : std::exchange(streams_, {}))
      stream->on_close();

   logi("session ended");
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
         loge("send loop: nghttp2_session_mem_send2 failed: {}",
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
         logd("send loop: writing {} bytes...", bytes_to_write);
         std::array<boost::capy::const_buffer, 2> seq{
            boost::capy::const_buffer(pending.data(), pending.size()),
            boost::capy::const_buffer(data, static_cast<std::size_t>(nread))};
         auto [ec, written] = co_await boost::capy::write(stream_, seq);
         pending.clear();
         if (ec)
         {
            logw("send loop: write error: {}", ec.message());
            break;
         }
         continue;
      }

      // Nothing buffered, nothing new: either we're done, or we wait to be poked by recv_loop()
      // (via start_write()) once it has fed nghttp2 more input.
      if (!nghttp2_session_want_read(session_) && !nghttp2_session_want_write(session_))
         break;

      logd("send loop: waiting...");
      write_ready_.clear();
      if (auto [ec] = co_await write_ready_.wait(); ec)
         break;
   }

   logd("send loop: done");
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
         logd("recv loop: {}, terminating session", ec.message());
         break;
      }

      logd("recv loop: read {} bytes", n);
      if (auto rv = nghttp2_session_mem_recv2(session_, buffer.data(), n); rv < 0)
      {
         loge("recv loop: nghttp2_session_mem_recv2 failed: {}",
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

   logd("recv loop: done");
   co_return boost::capy::io_result<>{};
}

// -------------------------------------------------------------------------------------------------

std::shared_ptr<Stream> Session::Impl::create_stream(std::int32_t id)
{
   auto stream = std::make_shared<Stream>(shared_from_this(), id);
   streams_.emplace(id, stream);
   return stream;
}

std::shared_ptr<Stream> Session::Impl::find_stream(std::int32_t id) const
{
   auto it = streams_.find(id);
   return it != streams_.end() ? it->second : nullptr;
}

void Session::Impl::close_stream(std::int32_t id)
{
   auto it = streams_.find(id);
   if (it == streams_.end())
      return;

   // Unblock anything still suspended in a read/write on this stream before dropping our own
   // reference -- a StreamReader/StreamWriter held by the user (or by handle_request()'s own
   // coroutine frame) can keep the Stream alive past this point regardless.
   it->second->on_close();
   streams_.erase(it);
}

void Session::Impl::dispatch_request(std::shared_ptr<Stream> stream)
{
   // Tracked via the same TaskGroup as the session itself (see Server::Impl::~Impl()), so a
   // request handler still mid-flight when the owning Server is destroyed gets cancelled and
   // joined too, not left dangling.
   auto& group = executor_.context().use_service<detail::TaskGroup>();
   group.spawn(executor_, handle_request(std::move(stream)));
}

// -------------------------------------------------------------------------------------------------

boost::capy::task<> Session::Impl::handle_request(std::shared_ptr<Stream> stream)
{
   auto self = shared_from_this(); // keep the session alive for the duration of the coroutine

   Session::Request request(stream->path(), Session::Reader(StreamReader(stream)));
   Session::Response response{Session::Writer(StreamWriter(stream)),
                              [self, stream](unsigned int status, const Session::Headers& headers)
   { return self->submit_response(stream, status, headers); }};

   if (handler_)
   {
      co_await handler_(std::move(request), std::move(response));
   }
   else
   {
      logw("[{}] handle_request: no handler configured, closing response empty", stream->id());
      [[maybe_unused]] auto result = co_await response.write_eof();
   }
}

// -------------------------------------------------------------------------------------------------

boost::capy::io_task<> Session::Impl::submit_response(std::shared_ptr<Stream> stream,
                                                      unsigned int status,
                                                      const Session::Headers& headers)
{
   auto status_str = std::to_string(status);
   std::vector<nghttp2_nv> nva;
   nva.reserve(1 + headers.size());
   nva.push_back(make_nv(":status", status_str));
   for (const auto& [name, value] : headers)
      nva.push_back(make_nv(name, value));

   nghttp2_data_provider2 prd{.source = {.ptr = stream.get()},
                              .read_callback = data_source_read_callback};
   if (nghttp2_submit_response2(session_, stream->id(), nva.data(), nva.size(), &prd))
   {
      loge("[{}] submit_response: nghttp2_submit_response2 failed", stream->id());
      co_return boost::capy::io_result<>{std::make_error_code(std::errc::invalid_argument)};
   }
   start_write();
   co_return boost::capy::io_result<>{};
}

// -------------------------------------------------------------------------------------------------

boost::capy::io_task<Session::Writer, Session::ClientResponse>
Session::Impl::submit_request(std::string_view path)
{
   if (role_ != Role::client)
      co_return {std::make_error_code(std::errc::operation_not_permitted), Session::Writer{},
                 Session::ClientResponse{Session::Reader{}, {}}};

   // Placeholder stream ID: nghttp2_submit_request2() below assigns the real one, and we need
   // *some* object to hand it as the data provider's source before that happens.
   auto stream = std::make_shared<Stream>(shared_from_this(), 0);

   nghttp2_data_provider2 prd{.source = {.ptr = stream.get()},
                              .read_callback = data_source_read_callback};
   std::string path_str(path);
   nghttp2_nv nva[]{
      make_nv(":method", "POST"),
      make_nv(":scheme", "http"),
      make_nv(":path", path_str),
      make_nv(":authority", "localhost"),
   };

   auto id = nghttp2_submit_request2(session_, nullptr, nva, std::size(nva), &prd, stream.get());
   if (id < 0)
   {
      loge("submit_request: nghttp2_submit_request2 failed: {}", nghttp2_strerror(id));
      co_return {std::make_error_code(std::errc::invalid_argument), Session::Writer{},
                 Session::ClientResponse{Session::Reader{}, {}}};
   }

   // nghttp2 now holds `stream.get()` as the data provider's source, so fix up its ID in place
   // rather than swapping in a different object.
   stream->set_id(id);
   streams_.emplace(id, stream);
   start_write();

   logi("[{}] submit_request: {}", id, path);
   co_return {std::error_code{}, Session::Writer(StreamWriter(stream)),
              Session::ClientResponse(Session::Reader(StreamReader(stream)),
                                      [stream] { return stream->status(); })};
}

// =================================================================================================

} // namespace nghttp2_corosio
