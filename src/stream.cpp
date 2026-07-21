#include "stream_impl.hpp"

#include "session_impl.hpp"

#include <nghttp2/nghttp2.h>

#include <algorithm>
#include <cstring>

namespace nghttp2_corosio
{

// =================================================================================================

void Stream::on_data(const std::uint8_t* data, std::size_t len)
{
   read_buffer_.insert(read_buffer_.end(), data, data + len);
   read_ready_.set();
}

void Stream::on_read_eof()
{
   logd("[{}] on_read_eof", log_prefix_);
   read_eof_ = true;
   read_ready_.set();
}

void Stream::on_close()
{
   logd("[{}] on_close", log_prefix_);
   closed_ = true;
   read_ready_.set();
   write_progress_.set();
}

// -------------------------------------------------------------------------------------------------

void Stream::consume(std::size_t n)
{
   nghttp2_session_consume_stream(session_->native_handle(), id_, n);
   session_->start_write(); // consuming may reopen the stream's flow-control window
}

// -------------------------------------------------------------------------------------------------

std::ptrdiff_t Stream::producer_callback(std::uint8_t* buf, std::size_t length,
                                         std::uint32_t* data_flags)
{
   logd("[{}] write callback (buffer size={} bytes)", log_prefix_, length);

   if (!write_pending_)
   {
      logd("[{}] write callback: nothing to send, DEFERRING", log_prefix_);
      write_deferred_ = true;
      return NGHTTP2_ERR_DEFERRED;
   }

   // Copy directly out of the caller's buffers (pending_ references them, doesn't own them) at
   // the current cumulative offset, walking however many of the descriptors are needed to fill
   // `length` or exhaust what's left.
   std::size_t copied = 0;
   std::size_t skip = write_offset_;
   for (std::size_t i = 0; i < pending_count_ && copied < length; ++i)
   {
      auto const& b = pending_[i];
      if (skip >= b.size())
      {
         skip -= b.size();
         continue;
      }
      auto const avail = b.size() - skip;
      auto const n = std::min(avail, length - copied);
      std::memcpy(buf + copied, static_cast<const std::uint8_t*>(b.data()) + skip, n);
      copied += n;
      skip = 0;
   }
   write_offset_ += copied;

   // write_some()/write() (write_eof_requested_ == false) complete right here, after this one
   // call, reporting whatever it actually copied -- genuine partial-write semantics, with
   // boost::capy::write() (see Stream::write()) looping back for the remainder if the caller
   // wants everything sent. write_eof(buffers) can't stop early like that: its contract is to
   // send everything and only then mark EOF, atomically, so it keeps waiting across as many
   // calls as it takes to fully drain pending_ first.
   bool const fully_drained = write_offset_ == pending_size_;
   if (fully_drained || !write_eof_requested_)
   {
      if (write_eof_requested_)
         *data_flags |= NGHTTP2_DATA_FLAG_EOF;

      // Reset synchronously, not just in write_impl() after write_progress_ wakes it back up:
      // nghttp2 can call this callback again for the same stream before that resumption actually
      // runs (it's posted, not immediate), and by then write_pending_ must already read false or
      // this returns another spurious zero-length, non-deferred completion -- flooding empty DATA
      // frames until the real resumption finally catches up.
      write_pending_ = false;
      write_progress_.set();
   }

   return static_cast<std::ptrdiff_t>(copied);
}

void Stream::resume_write() { nghttp2_session_resume_data(session_->native_handle(), id_); }

void Stream::poke_send_loop() { session_->start_write(); }

// =================================================================================================

} // namespace nghttp2_corosio
