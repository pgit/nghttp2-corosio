#pragma once

#include "nghttp2-corosio/logging.hpp"
#include "nghttp2-corosio/session.hpp"
#include "session_impl.hpp"

#include <boost/capy/buffers.hpp>
#include <boost/capy/buffers/buffer_copy.hpp>
#include <boost/capy/buffers/buffer_param.hpp>
#include <boost/capy/buffers/consuming_buffers.hpp>
#include <boost/capy/concept/read_stream.hpp>
#include <boost/capy/concept/write_stream.hpp>
#include <boost/capy/detail/buffer_array.hpp>
#include <boost/capy/error.hpp>
#include <boost/capy/ex/async_event.hpp>
#include <boost/capy/io_result.hpp>
#include <boost/capy/io_task.hpp>
#include <boost/capy/read.hpp>
#include <boost/capy/write.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace nghttp2_corosio
{

// =================================================================================================

/// Per-HTTP/2-stream bridge between nghttp2's synchronous callbacks and coroutine-native
/// read/write. One instance per stream, kept alive by whichever of {Session::Impl's stream map,
/// a StreamReader, a StreamWriter} still references it.
///
/// nghttp2 delivers body bytes via on_data_chunk_recv_callback (synchronous, called from
/// nghttp2_session_mem_recv2) and pulls outgoing body bytes via a nghttp2_data_provider2 read
/// callback (synchronous, called from nghttp2_session_mem_send2). Neither can suspend, so both
/// directions are bridged through an internal buffer plus a capy::async_event -- the same
/// building block Session::Impl::send_loop()/recv_loop() already use for the connection-level
/// case of "nghttp2 has nothing to send right now, wait to be poked".
class Stream : public std::enable_shared_from_this<Stream>
{
public:
   Stream(std::shared_ptr<Session::Impl> session, std::int32_t id)
      : session_(std::move(session)), id_(id), log_prefix_(session_->log_prefix(id_))
   {
      logd("[{}] Stream: ctor", log_prefix_);
   }

   ~Stream() { logd("[{}] Stream: dtor", log_prefix_); }

   std::int32_t id() const noexcept { return id_; }

   /// Log tag for this stream, e.g. "server.1". Cached at construction; refreshed by set_id() for
   /// a client-submitted request, whose real stream ID isn't known until after submission.
   const std::string& log_prefix() const noexcept { return log_prefix_; }

   /// Fixes up the stream ID once nghttp2_submit_request2() assigns the real one -- the client
   /// doesn't know it until after that call, but the Stream (as the data provider's source) has
   /// to exist before making it.
   void set_id(std::int32_t id) noexcept
   {
      id_ = id;
      log_prefix_ = session_->log_prefix(id_);
   }

   /// Set by on_header_callback() as the :path pseudo-header arrives (server sessions only).
   void set_path(std::string path) { path_ = std::move(path); }
   const std::string& path() const noexcept { return path_; }

   /// Set by on_header_callback() as a content-length header arrives -- request headers on a
   /// server session, response headers on a client session. Surfaced via
   /// Session::Request::content_length()/ClientResponse::content_length().
   void set_content_length(std::size_t n) noexcept { content_length_ = n; }
   std::optional<std::size_t> content_length() const noexcept { return content_length_; }

   /// Appends a header captured by on_header_callback() -- every header on the frame except the
   /// pseudo-headers already surfaced via their own dedicated accessors (path()/status()). Order
   /// preserving and duplicates included, since HTTP permits repeated header names (e.g.
   /// Set-Cookie). Surfaced via Session::Request::headers()/ClientResponse::headers().
   void add_header(std::string name, std::string value)
   {
      headers_.emplace_back(std::move(name), std::move(value));
   }
   const Session::Headers& headers() const noexcept { return headers_; }

   /// Set by on_header_callback() as the :status pseudo-header arrives (client sessions only,
   /// on the response HEADERS frame). Wakes anything suspended in status().
   void set_status(unsigned int status) noexcept
   {
      status_ = static_cast<int>(status);
      status_ready_.set();
   }

   /// Waits for the response's :status pseudo-header to arrive, or returns immediately if it
   /// already has. Also wakes -- with eof_error(), since no status is ever coming -- if the
   /// stream closes first (see on_close()), e.g. a server torn down mid-request. In practice this
   /// is always stream_truncated rather than eof: HTTP/2 delivers headers before any body data, so
   /// read_eof_ can't be set yet when a response's own status never arrived in the first place.
   boost::capy::io_task<unsigned int> status()
   {
      if (status_ < 0)
      {
         status_ready_.clear();
         if (auto [ec] = co_await status_ready_.wait(); ec)
            co_return {ec, 0u};
         if (status_ < 0)
            co_return {eof_error(), 0u};
      }
      co_return {{}, static_cast<unsigned int>(status_)};
   }

   // ----------------------------------------------------------------------------------------------
   // Read side, fed by on_data_chunk_recv_callback() and END_STREAM detection in
   // on_frame_recv_callback(). Consumers see this through StreamReader's read_some()/read().

   /// Delivers bytes from on_data_chunk_recv_callback(). If a read_some() call is currently
   /// parked with nothing buffered, copies straight into its destination buffers via read_sink_
   /// -- no intermediate storage at all. Only whatever doesn't fit there (or arrives when nothing
   /// is parked) gets copied into a new chunks_ entry. Either way, wakes a suspended read.
   void on_data(const std::uint8_t* data, std::size_t len);

   /// Marks the read side as ended (peer sent END_STREAM) and wakes a suspended read.
   void on_read_eof();

   /// Marks both directions as closed (on_stream_close_callback) and wakes anything suspended.
   void on_close();

   template <boost::capy::MutableBufferSequence MB>
   boost::capy::io_task<std::size_t> read_some(MB buffers)
   {
      logd("[{}] read_some:", log_prefix_);

      boost::capy::buffer_param bp(buffers);
      std::size_t total = 0;

      // Drain whatever is already queued, oldest first. Draining always starts at the front and
      // stops as soon as the destination is full, so at most the front chunk is ever partially
      // consumed -- front_offset_ alone tracks that, popped once exhausted. O(chunks)
      // buffer_copy() calls, never the O(bytes already delivered) shift that erasing from the
      // front of one flat vector would cost.
      while (!chunks_.empty())
      {
         auto dst = bp.data();
         if (dst.empty())
            break;
         auto& chunk = chunks_.front();
         boost::capy::const_buffer src(chunk.data() + front_offset_, chunk.size() - front_offset_);
         auto const n = boost::capy::buffer_copy(dst, src);
         bp.consume(n);
         front_offset_ += n;
         total += n;
         if (front_offset_ == chunk.size())
         {
            chunks_.pop_front();
            front_offset_ = 0;
         }
      }

      if (total > 0)
      {
         consume(total);
         logd("[{}] read_some: finished, {} bytes ({} chunks still buffered, eof_received={})",
              log_prefix_, total, chunks_.size(), read_eof_);
         co_return {{}, total};
      }

      if (read_eof_ || closed_)
      {
         logd("[{}] read_some: finished, 0 bytes pending, eof_received={}, closed={}", log_prefix_,
              read_eof_, closed_);
         co_return {eof_error(), 0};
      }

      // Nothing buffered: park here and publish `bp` via read_sink_ so on_data() can copy an
      // incoming chunk straight into the caller's buffers -- zero copies into chunks_ at all, in
      // the steady-state case where this call is already waiting when the next DATA frame lands.
      // Mirrors anyhttp's NGHttp2Stream::call_read_handler(), which delivers directly to a
      // pending read handler and only buffers what it can't hand off immediately.
      struct Sink : ReadSink
      {
         boost::capy::buffer_param<MB>& bp;
         std::size_t copied = 0;
         explicit Sink(boost::capy::buffer_param<MB>& bp) : bp(bp) {}
         std::size_t put(const std::uint8_t* data, std::size_t len) override
         {
            auto const n =
               boost::capy::buffer_copy(bp.data(), boost::capy::const_buffer(data, len));
            bp.consume(n);
            copied += n;
            return n;
         }
      } sink(bp);

      for (;;)
      {
         read_sink_ = &sink;
         read_ready_.clear();
         auto [ec] = co_await read_ready_.wait();
         read_sink_ = nullptr;

         if (sink.copied > 0)
         {
            consume(sink.copied);
            logd("[{}] read_some: finished, {} bytes delivered directly (eof_received={})",
                 log_prefix_, sink.copied, read_eof_);
            co_return {{}, sink.copied};
         }
         if (ec)
            co_return {ec, 0};
         if (read_eof_ || closed_)
         {
            logd("[{}] read_some: finished, 0 bytes pending, eof_received={}, closed={}",
                 log_prefix_, read_eof_, closed_);
            co_return {eof_error(), 0};
         }
      }
   }

   // ----------------------------------------------------------------------------------------------
   // Write side, pulled by nghttp2's data provider read callback (see producer_callback()).
   // Producers push through StreamWriter's write_some()/write()/write_eof().

   /// Invoked synchronously by nghttp2, via the data provider registered with
   /// nghttp2_submit_request2()/nghttp2_submit_response2(), when it wants more body bytes for a
   /// DATA frame. Returns NGHTTP2_ERR_DEFERRED (as a plain std::ptrdiff_t, to avoid needing
   /// nghttp2.h in this header) if nothing has been written yet.
   std::ptrdiff_t producer_callback(std::uint8_t* buf, std::size_t length,
                                    std::uint32_t* data_flags);

   /// A genuine partial write: completes as soon as producer_callback() has been invoked once,
   /// reporting however many bytes that single call actually pulled -- which may be less than
   /// `buffers`' full size if nghttp2's flow-control window closes partway through. Callers that
   /// want the rest sent too get that via write() below, not by write_some() looping internally.
   template <boost::capy::ConstBufferSequence CB>
   boost::capy::io_task<std::size_t> write_some(CB buffers)
   {
      // Not a coroutine: write_impl()'s parameter type converts `buffers` into a self-contained,
      // max_iovec_-capped array right here in the argument list, the same way corosio's
      // tls_stream::read_some()/do_read_some() split a template wrapper from a concrete-typed
      // virtual does. The array is copied by value into write_impl()'s own coroutine frame, so
      // there's no borrowed reference whose lifetime this function would need to keep alive.
      return write_impl(buffers, false);
   }

   template <boost::capy::ConstBufferSequence CB>
   boost::capy::io_task<std::size_t> write(CB buffers)
   {
      co_return co_await boost::capy::write(*this, buffers);
   }

   template <boost::capy::ConstBufferSequence CB>
   boost::capy::io_task<std::size_t> write_eof(CB buffers)
   {
      return write_impl(buffers, true);
   }

   boost::capy::io_task<> write_eof()
   {
      auto [ec, n] = co_await write_impl({}, true);
      std::ignore = n;
      co_return boost::capy::io_result<>{ec};
   }

private:
   void consume(std::size_t n);

   /// The error read_some() reports once nothing more is coming: `error::eof` if the peer sent a
   /// clean END_STREAM (read_eof_), or `error::stream_truncated` if the stream instead closed
   /// without one -- e.g. nghttp2 RST_STREAM-ing it after detecting the actual body didn't match
   /// an advertised content-length (see on_header_callback()'s "content-length" case in
   /// session.cpp). Checking read_eof_ first matters: a stream that finished cleanly and was
   /// *then* torn down (the ordinary case -- on_close() always runs after on_read_eof(), see
   /// Session::Impl::run()) must still report plain eof, not stream_truncated.
   std::error_code eof_error() const noexcept
   {
      return boost::capy::make_error_code(read_eof_ ? boost::capy::error::eof
                                                    : boost::capy::error::stream_truncated);
   }

   /// Not a template: write_some()/write_eof() above convert their arbitrary CB into this exact,
   /// self-contained array type at the call site -- mirroring corosio's
   /// tls_stream::do_read_some()/do_write_some(), which take the same kind of fixed-capacity
   /// array by value rather than an arbitrary buffer sequence. producer_callback() therefore only
   /// ever has to walk one concrete representation, capped at capy's iovec bound
   /// (boost::capy::detail::max_iovec_) by construction, not by a locally-reasserted invariant.
   boost::capy::io_task<std::size_t>
   write_impl(boost::capy::detail::const_buffer_array<boost::capy::detail::max_iovec_> buffers,
              bool eof)
   {
      if (closed_)
         co_return {boost::capy::make_error_code(boost::capy::error::eof), 0};

      // Copy the descriptors (pointer+size pairs, not the bytes they describe) into pending_ so
      // producer_callback() -- a plain member function, not part of this coroutine's frame -- can
      // reach them; `buffers` itself only lives as long as this frame stays suspended, which is
      // exactly as long as producer_callback() needs it, but it isn't reachable from there
      // directly. pending_cursor_ tracks how much of pending_ has been consumed so far.
      pending_ = buffers;
      pending_cursor_ = decltype(pending_cursor_){pending_};

      write_offset_ = 0;
      write_eof_requested_ = eof;
      write_pending_ = true;

      if (std::exchange(write_deferred_, false))
         resume_write();
      poke_send_loop();

      write_progress_.clear();
      auto [ec] = co_await write_progress_.wait();
      write_pending_ = false;
      // write_offset_ reports actual progress in both cases now: on success it's the whole
      // buffer for write_eof(), or whatever producer_callback() managed for write_some(); on
      // error/cancellation, it's the (possibly zero) partial progress made before that happened.
      co_return {ec, write_offset_};
   }

   void resume_write();
   void poke_send_loop();

   std::shared_ptr<Session::Impl> session_;
   std::int32_t id_;
   bool closed_ = false;
   std::string path_;
   std::optional<std::size_t> content_length_;
   Session::Headers headers_;
   std::string log_prefix_;

   // response status (client sessions only)
   int status_ = -1;
   boost::capy::async_event status_ready_;

   // read side
   //
   // Chunks of bytes that arrived (via on_data()) while no read_some() call was parked waiting
   // for them.
   std::deque<std::vector<std::uint8_t>> chunks_;

   // How much of chunks_.front() a prior read_some() call has already taken. Only the front
   // chunk can ever be partially consumed -- draining always proceeds front-to-back and stops as
   // soon as the destination is full -- so a single offset suffices; it resets to 0 whenever the
   // front chunk is fully drained and popped.
   std::size_t front_offset_ = 0;

   // Type-erased handle for a parked read_some() call's destination buffers. Set only while such
   // a call is suspended in read_ready_.wait() with chunks_ empty; on_data() uses it, when
   // non-null, to copy an incoming chunk directly into the caller's buffers, bypassing chunks_
   // entirely for whatever fits. See read_some()'s local `Sink` type for the concrete
   // implementation.
   struct ReadSink
   {
      virtual ~ReadSink() = default;
      virtual std::size_t put(const std::uint8_t* data, std::size_t len) = 0;
   };
   ReadSink* read_sink_ = nullptr;

   bool read_eof_ = false;
   boost::capy::async_event read_ready_;

   // write side
   //
   // pending_ owns a copy of the current write's buffer descriptors (pointer+size pairs, not the
   // bytes) -- see write_impl()'s doc comment. pending_cursor_ walks it across however many
   // producer_callback() invocations it takes to drain -- optional only because consuming_buffers
   // has no default state; it's always engaged by the time producer_callback() can run.
   boost::capy::detail::const_buffer_array<boost::capy::detail::max_iovec_> pending_;
   boost::capy::consuming_buffers<decltype(pending_)> pending_cursor_{pending_};
   std::size_t write_offset_ = 0; // bytes of pending_ consumed so far by producer_callback()
   bool write_pending_ = false;
   bool write_eof_requested_ = false;
   bool write_deferred_ = false;
   boost::capy::async_event write_progress_;
};

// =================================================================================================

/// Concrete ReadStream wrapping a Stream's read side. Type-erased into Session::Reader at the
/// public API boundary. Only read_some() is needed: Session::Request/ClientResponse::read()
/// composes it via the free boost::capy::read() algorithm rather than a forwarding member here.
class StreamReader
{
public:
   explicit StreamReader(std::shared_ptr<Stream> stream) : stream_(std::move(stream)) {}

   template <boost::capy::MutableBufferSequence MB>
   boost::capy::io_task<std::size_t> read_some(MB buffers)
   {
      return stream_->read_some(buffers);
   }

private:
   std::shared_ptr<Stream> stream_;
};

static_assert(boost::capy::ReadStream<StreamReader>);

/// Concrete WriteStream wrapping a Stream's write side. Type-erased into Session::Writer at the
/// public API boundary. Only write_some() is needed here: Session::Response/ClientRequest::write()
/// composes it via the free boost::capy::write() algorithm, and write_eof() bypasses Writer/
/// StreamWriter entirely -- Session::Impl binds it straight to the owning Stream (see
/// Session::Response::WriteEofFn's doc comment in session.hpp), since capy's WriteStream concept
/// has no notion of it.
class StreamWriter
{
public:
   explicit StreamWriter(std::shared_ptr<Stream> stream) : stream_(std::move(stream)) {}

   template <boost::capy::ConstBufferSequence CB>
   boost::capy::io_task<std::size_t> write_some(CB buffers)
   {
      return stream_->write_some(buffers);
   }

private:
   std::shared_ptr<Stream> stream_;
};

static_assert(boost::capy::WriteStream<StreamWriter>);

// =================================================================================================

} // namespace nghttp2_corosio
