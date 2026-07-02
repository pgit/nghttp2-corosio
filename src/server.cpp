#include "nghttp2-corosio/logging.hpp"
#include "nghttp2-corosio/server.hpp"
#include "server_impl.hpp"
#include "session_impl.hpp"

#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/io/any_stream.hpp>
#include <boost/corosio/ipv6_address.hpp>
#include <boost/corosio/tcp_socket.hpp>

#include <sstream>

namespace nghttp2_corosio
{

// =================================================================================================

Server::Impl::Impl(Config config)
   : config_(std::move(config))
   , acceptor_(ioc_, boost::corosio::endpoint(boost::corosio::ipv6_address(config_.listen_address),
                                              config_.port))
{
}

void Server::Impl::start() { boost::capy::run_async(ioc_.get_executor())(accept_loop()); }

boost::capy::task<> Server::Impl::accept_loop()
{
   auto ep = acceptor_.local_endpoint();
   logi("Listening on port {}", ep.port());

   for (;;)
   {
      boost::corosio::tcp_socket peer(ioc_);
      auto [ec] = co_await acceptor_.accept(peer);
      if (ec)
      {
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
      boost::capy::run_async(ioc_.get_executor())(session->run());
   }
}

// =================================================================================================

Server::Server(Config config) : impl_(std::make_shared<Impl>(std::move(config)))
{
   impl_->start();
}

Server::Server(Server&&) noexcept = default;
Server& Server::operator=(Server&&) noexcept = default;
Server::~Server() = default;

Server::executor_type Server::get_executor() const noexcept { return impl_->get_executor(); }

boost::corosio::endpoint Server::local_endpoint() const { return impl_->local_endpoint(); }

std::size_t Server::run() { return impl_->run(); }

void Server::stop() { impl_->stop(); }

// =================================================================================================

} // namespace nghttp2_corosio
