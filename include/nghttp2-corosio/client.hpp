#pragma once

#include "nghttp2-corosio/session.hpp"

#include <boost/capy/ex/any_executor.hpp>
#include <boost/capy/io_task.hpp>
#include <boost/corosio/endpoint.hpp>

#include <memory>

namespace nghttp2_corosio
{

// =================================================================================================

class Client
{
public:
   class Impl;

   explicit Client(boost::capy::any_executor executor);
   Client(Client&& other) noexcept;
   Client& operator=(Client&& other) noexcept;
   ~Client();

   using executor_type = boost::capy::any_executor;
   executor_type get_executor() const noexcept;

   /// Connects to `ep` and starts an HTTP/2 session on the resulting stream, driven by the same
   /// send/recv loop a server-side session uses. Doesn't submit any requests yet.
   boost::capy::io_task<Session> connect(boost::corosio::endpoint ep);

private:
   std::shared_ptr<Impl> impl_;
};

// =================================================================================================

} // namespace nghttp2_corosio
