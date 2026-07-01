#include "nghttp2-corosio/session.hpp"
#include "session_impl.hpp"

#include <boost/capy/write.hpp>

#include <iostream>

namespace nghttp2_corosio
{

// =================================================================================================

Session::Session(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}
Session::Session(Session&&) noexcept = default;
Session& Session::operator=(Session&&) noexcept = default;
Session::~Session() = default;

Session::executor_type Session::get_executor() const noexcept { return impl_->get_executor(); }

// =================================================================================================

boost::capy::task<> Session::Impl::run()
{
   auto self = shared_from_this(); // keep the session alive for the duration of the coroutine

   char buf[1024];
   std::size_t total_bytes = 0;

   for (;;)
   {
      auto [ec, n] = co_await stream_.read_some(boost::capy::mutable_buffer(buf, sizeof(buf)));
      if (ec)
         break;

      auto [wec, wn] = co_await boost::capy::write(stream_, boost::capy::const_buffer(buf, n));
      if (wec)
         break;

      total_bytes += static_cast<std::size_t>(wn);
   }

   std::cout << "Session ended, echoed " << total_bytes << " bytes\n";
}

// =================================================================================================

} // namespace nghttp2_corosio
