#pragma once

#include <boost/capy/buffers.hpp>
#include <boost/capy/detail/buffer_array.hpp>
#include <boost/capy/io_task.hpp>
#include <boost/capy/read.hpp>

#include <memory>
#include <span>
#include <utility>

namespace nghttp2_corosio
{

// =================================================================================================

/// Type-erased wrapper for a readable body. Deliberately *not* built on capy's `any_read_stream`:
/// that wrapper starts the underlying read from inside `await_ready()` (its `await_suspend()`
/// just resumes an already-constructed coroutine, unlike `any_write_stream`'s, which constructs
/// there). Generic awaitable adapters aren't required to call `await_ready()` on an inner
/// awaitable before driving its `await_suspend()` directly -- see `IoAwaitable`'s own
/// conforming-signature example, which puts all setup in `await_suspend` and leaves
/// `await_ready()` a side-effect-free `return false`. corosio's `timeout()` is exactly such an
/// adapter (it always arms its own timer first), so racing `any_read_stream::read_some()` against
/// a deadline resumes a coroutine frame that was never constructed -- undefined behavior, caught
/// by valgrind/ASan as a read of uninitialized memory immediately followed by a segfault.
///
/// A plain virtual call sidesteps this: `read_some()` below always returns a genuine, already-
/// running `io_task<>` (the same coroutine type `Stream::read_some()` itself returns), whose own
/// awaiter is capy's core primitive -- not the newer cached-awaitable wrapper -- so it has no such
/// gap between `await_ready()` and `await_suspend()`.
class any_read_source
{
public:
   any_read_source() = default;

   template <typename S>
      requires (!std::same_as<std::decay_t<S>, any_read_source>)
   explicit any_read_source(S s) : model_(std::make_unique<Model<S>>(std::move(s)))
   {
   }

   any_read_source(any_read_source&&) noexcept = default;
   any_read_source& operator=(any_read_source&&) noexcept = default;

   bool has_value() const noexcept { return model_ != nullptr; }
   explicit operator bool() const noexcept { return has_value(); }

   template <boost::capy::MutableBufferSequence MB>
   boost::capy::io_task<std::size_t> read_some(MB buffers)
   {
      // Must be a coroutine, not a plain forwarding function: ba's lifetime needs to span the
      // co_await below (it's read by the callee, possibly after suspending) -- see
      // any_write_sink.hpp's write_eof() for the same reasoning.
      boost::capy::detail::mutable_buffer_array<boost::capy::detail::max_iovec_> ba(buffers);
      co_return co_await model_->read_some(ba.to_span());
   }

   template <boost::capy::MutableBufferSequence MB>
   boost::capy::io_task<std::size_t> read(MB buffers)
   {
      return boost::capy::read(*this, buffers);
   }

private:
   struct Base
   {
      virtual ~Base() = default;
      virtual boost::capy::io_task<std::size_t> read_some(
         std::span<boost::capy::mutable_buffer const> buffers) = 0;
   };

   template <typename S>
   struct Model : Base
   {
      S s;
      explicit Model(S s_) : s(std::move(s_)) {}

      boost::capy::io_task<std::size_t> read_some(
         std::span<boost::capy::mutable_buffer const> buffers) override
      {
         return s.read_some(buffers);
      }
   };

   std::unique_ptr<Base> model_;
};

// =================================================================================================

} // namespace nghttp2_corosio
