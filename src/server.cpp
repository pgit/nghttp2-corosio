#include "nghttp2-corosio/server.hpp"
#include "nghttp2-corosio/logging.hpp"
#include "server_impl.hpp"
#include "session_impl.hpp"
#include "task_group.hpp"

#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/ex/this_coro.hpp>
#include <boost/capy/io/any_stream.hpp>
#include <boost/corosio/ipv6_address.hpp>
#include <boost/corosio/tcp_socket.hpp>

#include <sstream>

namespace nghttp2_corosio
{

// =================================================================================================

Server::Impl::Impl(Config config)
   : config_(std::move(config)),
     acceptor_(ioc_, boost::corosio::endpoint(boost::corosio::ipv6_address(config_.listen_address),
                                              config_.port))
{
}

Server::Impl::~Impl()
{
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
      if (ioc_.poll() == 0)
         ioc_.run_one();
   }
}

void Server::Impl::start()
{
   auto& group = ioc_.use_service<detail::TaskGroup>();
   group.spawn(ioc_.get_executor(), accept_loop());
}

boost::capy::task<> Server::Impl::accept_loop()
{
   auto ep = acceptor_.local_endpoint();
   logi("Listening on port {}", ep.port());

   auto& group = ioc_.use_service<detail::TaskGroup>();
   auto token = co_await boost::capy::this_coro::stop_token;

   for (;;)
   {
      boost::corosio::tcp_socket peer(ioc_);
      auto [ec] = co_await acceptor_.accept(peer);
      if (ec)
      {
         if (token.stop_requested())
            break;
         logw("Accept error: {}", ec.message());
         continue;
      }

      // corosio's address types only support operator<<, not std::formatter, so route through a
      // stream to build the string logi() can format.
      auto remote = peer.remote_endpoint();
      std::ostringstream address;
      if (remote.is_v4())
         address << remote.v4_address();
      else
         address << remote.v6_address();
      logi("Connection from {}:{}", address.str(), remote.port());

      // TODO: wrap `peer` in a TLS stream (corosio::openssl_stream / wolfssl_stream) here once
      // TLS support is added, before erasing it into `any_stream` below.
      boost::capy::any_stream stream(std::move(peer));

      auto session = std::make_shared<Session::Impl>(ioc_.get_executor(), std::move(stream),
                                                     Session::Impl::Role::server, config_.handler);
      group.spawn(ioc_.get_executor(), session->run());
   }
}

// =================================================================================================

Server::Server(Config config) : impl_(std::make_shared<Impl>(std::move(config))) { impl_->start(); }

Server::Server(Server&&) noexcept = default;
Server& Server::operator=(Server&&) noexcept = default;
Server::~Server() = default;

Server::executor_type Server::get_executor() const noexcept { return impl_->get_executor(); }

boost::corosio::endpoint Server::local_endpoint() const { return impl_->local_endpoint(); }

std::size_t Server::run() { return impl_->run(); }

void Server::stop() { impl_->stop(); }

// =================================================================================================

} // namespace nghttp2_corosio
