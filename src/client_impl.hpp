#pragma once

#include "nghttp2-corosio/client.hpp"
#include "nghttp2-corosio/session.hpp"

#include <boost/capy/ex/any_executor.hpp>
#include <boost/capy/io_task.hpp>
#include <boost/corosio/endpoint.hpp>

namespace nghttp2_corosio
{

// =================================================================================================

class Client::Impl
{
public:
   explicit Impl(boost::capy::any_executor executor) : executor_(std::move(executor)) {}

   boost::capy::any_executor get_executor() const noexcept { return executor_; }

   /// Connects a fresh tcp_socket to `ep`, optionally TLS-wraps it (see Client::connect()), wraps
   /// the result in an any_stream, and spawns a client-role Session::Impl::run() on it -- the
   /// counterpart to Server::Impl::accept_loop() spawning a server-role one per accepted
   /// connection.
   boost::capy::io_task<Session> connect(boost::corosio::endpoint ep,
                                          std::optional<boost::corosio::tls_context> tls,
                                          std::string hostname);

private:
   boost::capy::any_executor executor_;
};

// =================================================================================================

} // namespace nghttp2_corosio
