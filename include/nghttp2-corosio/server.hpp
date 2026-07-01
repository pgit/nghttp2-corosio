#pragma once

#include <boost/corosio/endpoint.hpp>
#include <boost/corosio/io_context.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace nghttp2_corosio
{

// =================================================================================================

struct Config
{
   std::string listen_address = "::";
   std::uint16_t port = 8080;
};

// =================================================================================================

class Server
{
public:
   class Impl;

   explicit Server(Config config = {});
   Server(Server&& other) noexcept;
   Server& operator=(Server&& other) noexcept;
   ~Server();

   using executor_type = boost::corosio::io_context::executor_type;
   executor_type get_executor() const noexcept;

   boost::corosio::endpoint local_endpoint() const;

   /// Runs the server's io_context until stopped. Blocks the calling thread.
   std::size_t run();

private:
   std::shared_ptr<Impl> impl_;
};

// =================================================================================================

} // namespace nghttp2_corosio
