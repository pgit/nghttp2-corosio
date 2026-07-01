#pragma once

#include "nghttp2-corosio/session.hpp"

#include <boost/capy/io/any_stream.hpp>
#include <boost/capy/task.hpp>

#include <memory>

namespace nghttp2_corosio
{

// =================================================================================================

class Session::Impl : public std::enable_shared_from_this<Session::Impl>
{
public:
   Impl(boost::capy::any_executor executor, boost::capy::any_stream stream)
      : executor_(std::move(executor)), stream_(std::move(stream))
   {
   }

   boost::capy::any_executor get_executor() const noexcept { return executor_; }

   /// Placeholder session body: echoes back whatever the peer sends. Will be replaced by the
   /// nghttp2-driven HTTP/2 session logic.
   boost::capy::task<> run();

private:
   boost::capy::any_executor executor_;
   boost::capy::any_stream stream_;
};

// =================================================================================================

} // namespace nghttp2_corosio
