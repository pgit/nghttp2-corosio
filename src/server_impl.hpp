#pragma once

#include "nghttp2-corosio/server.hpp"
#include "nghttp2-corosio/session.hpp"

#include <boost/capy/task.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/corosio/tcp_acceptor.hpp>

#include <memory>

namespace nghttp2_corosio
{

// =================================================================================================

class Server::Impl : public std::enable_shared_from_this<Server::Impl>
{
public:
   explicit Impl(Config config);

   /// Structured teardown: cancels the accept loop and every session (and request handler) ever
   /// spawned on this Server's io_context -- whether accepted by this Server or connected via a
   /// Client sharing its executor -- then drains the io_context (running it synchronously, right
   /// here, before any member starts destructing) until all of them have actually unwound. This
   /// makes it safe to fall through to the automatic member destruction below: no coroutine frame
   /// is left suspended for ~io_context() to tear down mid-flight. Ungraceful by design (in-flight
   /// request bodies are abandoned, not drained) since a destructor can't co_await; see
   /// detail::TaskGroup for how cancellation is tracked and propagated.
   ~Impl();

   void start();
   std::size_t run() { return ioc_.run(); }
   void stop() { ioc_.stop(); }

   const Config& config() const noexcept { return config_; }
   Server::executor_type get_executor() const noexcept { return ioc_.get_executor(); }
   boost::corosio::endpoint local_endpoint() const noexcept { return acceptor_.local_endpoint(); }

private:
   boost::capy::task<> accept_loop();

   Config config_;
   boost::corosio::io_context ioc_;
   boost::corosio::tcp_acceptor acceptor_;
};

// =================================================================================================

} // namespace nghttp2_corosio
