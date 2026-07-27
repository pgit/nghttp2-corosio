#pragma once

#include "nghttp2-corosio/session.hpp"

#include <boost/capy/ex/any_executor.hpp>
#include <boost/capy/io_task.hpp>
#include <boost/corosio/endpoint.hpp>
#include <boost/corosio/tls_context.hpp>

#include <memory>
#include <optional>
#include <string>

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
   ///
   /// If `tls` is set, the connection is TLS-wrapped first: `hostname`, if non-empty, is sent as
   /// the SNI hostname and matched against the peer certificate. ALPN must then negotiate "h2" or
   /// the connection is refused. Unset `tls` connects in plaintext (h2c), as before.
   boost::capy::io_task<Session> connect(boost::corosio::endpoint ep,
                                          std::optional<boost::corosio::tls_context> tls = std::nullopt,
                                          std::string hostname = {});

private:
   std::shared_ptr<Impl> impl_;
};

// =================================================================================================

} // namespace nghttp2_corosio
