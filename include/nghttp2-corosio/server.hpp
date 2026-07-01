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

   /// Causes a concurrent run() (on another thread) to return as soon as possible. Mainly useful
   /// for tests that run the server on a background thread and need to shut it down afterwards.
   ///
   /// Known issue: if a session hasn't fully wound down (e.g. its peer disconnected but the final
   /// GOAWAY hasn't been flushed yet), destroying this Server afterwards can hang or corrupt
   /// memory -- see the comment on Session::Impl::send_loop() in session.cpp. Safe to call once no
   /// sessions are in flight (e.g. right after construction, before accepting any connections).
   void stop();

private:
   std::shared_ptr<Impl> impl_;
};

// =================================================================================================

} // namespace nghttp2_corosio
