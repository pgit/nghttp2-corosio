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
   read_eof_ = true;
   read_ready_.set();
}

void Stream::on_close()
{
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
   if (!write_pending_)
   {
      write_deferred_ = true;
      return NGHTTP2_ERR_DEFERRED;
   }

   std::size_t copied = 0;
   if (write_offset_ < pending_write_.size())
   {
      copied = std::min(length, pending_write_.size() - write_offset_);
      std::memcpy(buf, pending_write_.data() + write_offset_, copied);
      write_offset_ += copied;
   }

   if (write_offset_ == pending_write_.size())
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
