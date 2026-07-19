#pragma once

#include "nghttp2-corosio/session.hpp"

#include <boost/corosio/endpoint.hpp>
#include <boost/corosio/io_context.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace nghttp2_corosio
{

// =================================================================================================

/// Handles every incoming request on the server -- there's no built-in routing, so a handler that
/// wants to do different things for different paths inspects `request.path()` itself and muxes
/// accordingly. `response` starts out as an unsubmitted 200 with no extra headers; a handler that
/// wants to customize either calls `response.status()`/`response.set()` before its first write (or
/// an explicit `response.submit()`) -- see server_main.cpp for a streaming echo handler.
using RequestHandler =
   std::function<boost::capy::task<>(Session::Request request, Session::Response response)>;

// =================================================================================================

struct Config
{
   std::string listen_address = "::";
   std::uint16_t port = 8080;

   /// Called once per incoming request. If unset, every request gets an empty 200 response.
   RequestHandler handler;
};

// =================================================================================================

class Server
{
public:
   class Impl;

   explicit Server(Config config = {});
   Server(Server&& other) noexcept;
   Server& operator=(Server&& other) noexcept;

   /// Cancels the accept loop, every session this Server accepted, every in-flight request
   /// handler, and -- since sessions register into a context-wide registry, see
   /// detail::TaskGroup in src/task_group.hpp -- any Client session sharing this Server's
   /// executor too. Then drains the io_context synchronously (running it right here) until all
   /// of them have actually finished, before any member -- in particular the io_context itself
   /// -- starts destructing. Ungraceful by design: in-flight request bodies are abandoned, not
   /// drained, since a destructor can't co_await. Always safe to call, regardless of what's in
   /// flight or whether stop() was called first.
   ~Server();

   using executor_type = boost::corosio::io_context::executor_type;
   executor_type get_executor() const noexcept;

   boost::corosio::endpoint local_endpoint() const;

   /// Runs the server's io_context until stopped. Blocks the calling thread.
   std::size_t run();

   /// Causes a concurrent run() (e.g. on another thread, or a synchronous run() call further down
   /// the same call stack) to return as soon as possible. This only stops the scheduler loop --
   /// it doesn't cancel the accept loop or any session; ~Server() does that (see above).
   void stop();

private:
   std::shared_ptr<Impl> impl_;
};

// =================================================================================================

} // namespace nghttp2_corosio
