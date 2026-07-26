#include "nghttp2-corosio/server.hpp"
#include "nghttp2-corosio/formatter.hpp"
#include "nghttp2-corosio/logging.hpp"
#include "server_impl.hpp"
#include "session_impl.hpp"
#include "task_group.hpp"

#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/ex/this_coro.hpp>
#include <boost/capy/io/any_stream.hpp>
#include <boost/corosio/ipv6_address.hpp>
#include <boost/corosio/socket_option.hpp>
#include <boost/corosio/tcp_socket.hpp>

namespace capy = boost::capy;
namespace corosio = boost::corosio;

namespace nghttp2_corosio
{

// =================================================================================================

Server::Impl::Impl(Config config)
   : config_(std::move(config)),
     acceptor_(ioc_, corosio::endpoint(corosio::ipv6_address(config_.listen_address), config_.port))
{
   logi("Server: ctor");
}

Server::Impl::~Impl()
{
   logi("Server: destroy");

   auto& group = ioc_.use_service<detail::TaskGroup>();
   group.request_stop();

   // Drain synchronously: run the io_context right here, before any member below starts
   // destructing, until every tracked coroutine (accept_loop, every session, every in-flight
   // request handler) has actually completed -- not merely been asked to. restart() undoes
   // whatever stop() call got us here (the scheduler refuses to process anything once stopped,
   // see io_context::stop()'s docs) and is harmless to call redundantly on every iteration.
   while (group.count() > 0)
   {
      ioc_.restart();
      std::println(
         "--- ~ ------------------------------------------------------------------------");
      if (ioc_.poll() == 0)
      {
         ioc_.run_one();
         std::println(
            "--- ~ ------------------------------------------------------------------------");
      }
   }

   logi("Server: dtor");
   std::println("------------------------------------------------------------------------------");
}

void Server::Impl::start()
{
   auto& group = ioc_.use_service<detail::TaskGroup>();
   group.spawn(ioc_.get_executor(), accept_loop());
}

std::size_t Server::Impl::run()
{
   if (log_level() > LogLevel::debug)
      return ioc_.run();

   std::size_t i = 0;
   using namespace std::chrono;
   auto t0 = steady_clock::now();
   for (i = 0; ioc_.run_one(); ++i)
   {
      auto t1 = steady_clock::now();
      auto dt = duration_cast<milliseconds>(t1 - t0);
      t0 = t1;
      if (dt < 10ms)
         std::println(
            "--- {} ------------------------------------------------------------------------", i);
      else
         std::println("\x1b[1;31m--- {} ({}) "
                      "----------------------------------------------------------------\x1b[0m",
                      i, dt);
   }
   return i;
}

void Server::Impl::stop() { ioc_.stop(); }

capy::task<> Server::Impl::accept_loop()
{
   auto ep = acceptor_.local_endpoint();
   logi("Server: listening on {}", ep);

   auto& group = ioc_.use_service<detail::TaskGroup>();
   auto token = co_await capy::this_coro::stop_token;

   for (;;)
   {
      corosio::tcp_socket peer(ioc_);
      auto [ec] = co_await acceptor_.accept(peer);
      if (ec)
      {
         if (token.stop_requested())
            break;
         logw("accept: {}", ec.message());
         continue;
      }

      // HTTP/2 is a request/response protocol with many small frames (HEADERS, WINDOW_UPDATE,
      // small DATA chunks); leaving Nagle's algorithm on lets those sit buffered waiting to
      // coalesce, which combined with the peer's delayed ACKs stalls unpipelined request/response
      // round trips by tens of milliseconds.
      peer.set_option(corosio::socket_option::no_delay(true));

      logi("[\x1b[1;31mserver\x1b[0m] [{}] new connection", peer.remote_endpoint());

      // TODO: wrap `peer` in a TLS stream (corosio::openssl_stream / wolfssl_stream) here once
      // TLS support is added, before erasing it into `any_stream` below.
      capy::any_stream stream(std::move(peer));

      auto session = std::make_shared<Session::Impl>(ioc_.get_executor(), std::move(stream),
                                                     Session::Impl::Role::server, config_.handler);
      group.spawn(ioc_.get_executor(), session->run());
   }

   logi("accept loop: done");
}

// =================================================================================================

Server::Server(Config config) : impl_(std::make_shared<Impl>(std::move(config))) { impl_->start(); }

Server::Server(Server&&) noexcept = default;
Server& Server::operator=(Server&&) noexcept = default;
Server::~Server() = default;

Server::executor_type Server::get_executor() const noexcept { return impl_->get_executor(); }

corosio::endpoint Server::local_endpoint() const { return impl_->local_endpoint(); }

std::size_t Server::run() { return impl_->run(); }

void Server::stop() { impl_->stop(); }

// =================================================================================================

} // namespace nghttp2_corosio
