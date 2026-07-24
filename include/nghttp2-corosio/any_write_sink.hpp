#pragma once

#include <boost/capy/buffers.hpp>
#include <boost/capy/detail/buffer_array.hpp>
#include <boost/capy/io/any_write_stream.hpp>
#include <boost/capy/io_task.hpp>
#include <boost/capy/write.hpp>

#include <memory>
#include <span>
#include <utility>

namespace nghttp2_corosio
{

// =================================================================================================

/// Type-erased wrapper for a writable body that, unlike capy's `any_write_stream`, can also signal
/// end-of-stream: `write_eof()` completes a write and tells the underlying sink no more bytes are
/// coming (the HTTP/2 `END_STREAM` flag). capy dropped this concept (formerly `WriteSink`) as
/// unused by any of its own algorithms; this project still needs it, so it's vendored here as a
/// thin wrapper around `any_write_stream` (which supplies `write_some()`/`write()`) plus a small
/// virtual dispatch for `write_eof()`, which any wrapped type must additionally provide.
class any_write_sink
{
public:
   any_write_sink() = default;

   template <typename S>
      requires (!std::same_as<std::decay_t<S>, any_write_sink>)
   explicit any_write_sink(S s) : model_(std::make_unique<Model<S>>(std::move(s)))
   {
   }

   any_write_sink(any_write_sink&&) noexcept = default;
   any_write_sink& operator=(any_write_sink&&) noexcept = default;

   bool has_value() const noexcept { return model_ != nullptr; }
   explicit operator bool() const noexcept { return has_value(); }

   template <boost::capy::ConstBufferSequence CB>
   auto write_some(CB buffers)
   {
      return model_->stream.write_some(std::move(buffers));
   }

   template <boost::capy::ConstBufferSequence CB>
   boost::capy::io_task<std::size_t> write(CB buffers)
   {
      return boost::capy::write(model_->stream, std::move(buffers));
   }

   template <boost::capy::ConstBufferSequence CB>
   boost::capy::io_task<std::size_t> write_eof(CB buffers)
   {
      // Must be a coroutine, not a plain forwarding function: ba's lifetime needs to span the
      // co_await below (it's read by the callee, possibly after suspending), and a coroutine
      // frame -- unlike a local in a function that returns before the callee's task is awaited
      // -- stays alive for exactly that long.
      boost::capy::detail::const_buffer_array<boost::capy::detail::max_iovec_> ba(buffers);
      co_return co_await model_->write_eof(ba.to_span());
   }

   boost::capy::io_task<> write_eof() { return model_->write_eof(); }

private:
   struct Base
   {
      boost::capy::any_write_stream stream;
      virtual ~Base() = default;
      virtual boost::capy::io_task<std::size_t> write_eof(
         std::span<boost::capy::const_buffer const> buffers) = 0;
      virtual boost::capy::io_task<> write_eof() = 0;
   };

   template <typename S>
   struct Model : Base
   {
      S s;
      explicit Model(S s_) : s(std::move(s_)) { stream = boost::capy::any_write_stream(&s); }

      boost::capy::io_task<std::size_t> write_eof(
         std::span<boost::capy::const_buffer const> buffers) override
      {
         return s.write_eof(buffers);
      }

      boost::capy::io_task<> write_eof() override { return s.write_eof(); }
   };

   std::unique_ptr<Base> model_;
};

// =================================================================================================

} // namespace nghttp2_corosio
