#pragma once

#include "nghttp2-corosio/session.hpp"

#include <boost/capy/buffers.hpp>
#include <boost/capy/buffers/buffer_copy.hpp>
#include <boost/capy/concept/read_source.hpp>
#include <boost/capy/concept/write_sink.hpp>
#include <boost/capy/error.hpp>
#include <boost/capy/ex/async_event.hpp>
#include <boost/capy/io_result.hpp>
#include <boost/capy/io_task.hpp>
#include <boost/capy/read.hpp>
#include <boost/capy/write.hpp>

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
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
      : session_(std::move(session)), id_(id)
   {
   }

   std::int32_t id() const noexcept { return id_; }

   /// Fixes up the stream ID once nghttp2_submit_request2() assigns the real one -- the client
   /// doesn't know it until after that call, but the Stream (as the data provider's source) has
   /// to exist before making it.
   void set_id(std::int32_t id) noexcept { id_ = id; }

   /// Set by on_header_callback() as the :path pseudo-header arrives (server sessions only).
   void set_path(std::string path) { path_ = std::move(path); }
   const std::string& path() const noexcept { return path_; }

   /// Set by on_header_callback() as the :status pseudo-header arrives (client sessions only,
   /// on the response HEADERS frame). Wakes anything suspended in status().
   void set_status(unsigned int status) noexcept
   {
      status_ = static_cast<int>(status);
      status_ready_.set();
   }

   /// Waits for the response's :status pseudo-header to arrive, or returns immediately if it
   /// already has.
   boost::capy::io_task<unsigned int> status()
   {
      if (status_ < 0)
      {
         status_ready_.clear();
         if (auto [ec] = co_await status_ready_.wait(); ec)
            co_return {ec, 0u};
      }
      co_return {{}, static_cast<unsigned int>(status_)};
   }

   // ----------------------------------------------------------------------------------------------
   // Read side, fed by on_data_chunk_recv_callback() and END_STREAM detection in
   // on_frame_recv_callback(). Consumers see this through StreamReader's read_some()/read().

   /// Appends bytes delivered by on_data_chunk_recv_callback() and wakes a suspended read.
   void on_data(const std::uint8_t* data, std::size_t len);

   /// Marks the read side as ended (peer sent END_STREAM) and wakes a suspended read.
   void on_read_eof();

   /// Marks both directions as closed (on_stream_close_callback) and wakes anything suspended.
   void on_close();

   template <boost::capy::MutableBufferSequence MB>
   boost::capy::io_task<std::size_t> read_some(MB buffers)
   {
      for (;;)
      {
         if (!read_buffer_.empty())
         {
            auto copied = boost::capy::buffer_copy(
               buffers, boost::capy::const_buffer(read_buffer_.data(), read_buffer_.size()));
            read_buffer_.erase(read_buffer_.begin(), read_buffer_.begin() + copied);
            consume(copied);
            co_return {{}, copied};
         }
         if (read_eof_ || closed_)
            co_return {boost::capy::make_error_code(boost::capy::error::eof), 0};

         read_ready_.clear();
         if (auto [ec] = co_await read_ready_.wait(); ec)
            co_return {ec, 0};
      }
   }

   template <boost::capy::MutableBufferSequence MB>
   boost::capy::io_task<std::size_t> read(MB buffers)
   {
      co_return co_await boost::capy::read(*this, buffers);
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
      co_return co_await write_impl(buffers, false);
   }

   template <boost::capy::ConstBufferSequence CB>
   boost::capy::io_task<std::size_t> write(CB buffers)
   {
      co_return co_await boost::capy::write(*this, buffers);
   }

   template <boost::capy::ConstBufferSequence CB>
   boost::capy::io_task<std::size_t> write_eof(CB buffers)
   {
      co_return co_await write_impl(buffers, true);
   }

   boost::capy::io_task<> write_eof()
   {
      auto [ec, n] = co_await write_impl(boost::capy::const_buffer{}, true);
      (void)n;
      co_return boost::capy::io_result<>{ec};
   }

private:
   void consume(std::size_t n);

   template <boost::capy::ConstBufferSequence CB>
   boost::capy::io_task<std::size_t> write_impl(CB buffers, bool eof)
   {
      if (closed_)
         co_return {boost::capy::make_error_code(boost::capy::error::eof), 0};

      // Reference the caller's buffers, not a copy of their bytes: the WriteSink contract
      // already requires `buffers` to stay alive until this co_await completes (see
      // boost::capy::write()'s own documented precondition), so producer_callback() can read
      // straight out of the caller's memory across however many invocations it takes. Only the
      // (small, fixed-count) buffer *descriptors* are copied here, never the data they describe.
      pending_count_ = 0;
      pending_size_ = 0;
      for (auto it = boost::capy::begin(buffers); it != boost::capy::end(buffers); ++it)
      {
         boost::capy::const_buffer b(*it);
         if (b.size() == 0)
            continue;
         assert(pending_count_ < pending_.size());
         pending_[pending_count_++] = b;
         pending_size_ += b.size();
      }

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

   // response status (client sessions only)
   int status_ = -1;
   boost::capy::async_event status_ready_;

   // read side
   std::vector<std::uint8_t> read_buffer_;
   bool read_eof_ = false;
   boost::capy::async_event read_ready_;

   // write side
   //
   // pending_ holds a *view* onto the current write's buffer descriptors (pointer+size pairs),
   // not a copy of the bytes -- see write_impl()'s doc comment. Its fixed capacity matches
   // capy's any_write_sink, which never hands Stream a scatter/gather sequence longer than its
   // own iovec bound (boost::capy::detail::max_iovec_, 16 at the time of writing).
   static constexpr std::size_t max_pending_buffers_ = 16;
   std::array<boost::capy::const_buffer, max_pending_buffers_> pending_{};
   std::size_t pending_count_ = 0;
   std::size_t pending_size_ = 0; // total bytes across pending_[0, pending_count_)
   std::size_t write_offset_ = 0; // bytes of pending_ consumed so far by producer_callback()
   bool write_pending_ = false;
   bool write_eof_requested_ = false;
   bool write_deferred_ = false;
   boost::capy::async_event write_progress_;
};

// =================================================================================================

/// Concrete ReadSource wrapping a Stream's read side. Type-erased into Session::Reader at the
/// public API boundary.
class StreamReader
{
public:
   explicit StreamReader(std::shared_ptr<Stream> stream) : stream_(std::move(stream)) {}

   template <boost::capy::MutableBufferSequence MB>
   boost::capy::io_task<std::size_t> read_some(MB buffers)
   {
      return stream_->read_some(buffers);
   }

   template <boost::capy::MutableBufferSequence MB>
   boost::capy::io_task<std::size_t> read(MB buffers)
   {
      return stream_->read(buffers);
   }

private:
   std::shared_ptr<Stream> stream_;
};

static_assert(boost::capy::ReadSource<StreamReader>);

/// Concrete WriteSink wrapping a Stream's write side. Type-erased into Session::Writer at the
/// public API boundary.
class StreamWriter
{
public:
   explicit StreamWriter(std::shared_ptr<Stream> stream) : stream_(std::move(stream)) {}

   template <boost::capy::ConstBufferSequence CB>
   boost::capy::io_task<std::size_t> write_some(CB buffers)
   {
      return stream_->write_some(buffers);
   }

   template <boost::capy::ConstBufferSequence CB>
   boost::capy::io_task<std::size_t> write(CB buffers)
   {
      return stream_->write(buffers);
   }

   template <boost::capy::ConstBufferSequence CB>
   boost::capy::io_task<std::size_t> write_eof(CB buffers)
   {
      return stream_->write_eof(buffers);
   }

   boost::capy::io_task<> write_eof() { return stream_->write_eof(); }

private:
   std::shared_ptr<Stream> stream_;
};

static_assert(boost::capy::WriteSink<StreamWriter>);

// =================================================================================================

} // namespace nghttp2_corosio
