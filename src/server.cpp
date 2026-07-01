#include "nghttp2-corosio/server.hpp"
#include "server_impl.hpp"

#include <boost/capy/ex/run_async.hpp>
#include <boost/corosio/ipv6_address.hpp>
#include <boost/corosio/tcp_socket.hpp>

#include <iostream>

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
   std::cout << "Listening on port " << ep.port() << "\n";

   for (;;)
   {
      boost::corosio::tcp_socket peer(ioc_);
      auto [ec] = co_await acceptor_.accept(peer);
      if (ec)
      {
         std::cout << "Accept error: " << ec.message() << "\n";
         continue;
      }

      auto remote = peer.remote_endpoint();
      std::cout << "Connection from ";
      if (remote.is_v4())
         std::cout << remote.v4_address();
      else
         std::cout << remote.v6_address();
      std::cout << ":" << remote.port() << "\n";
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

// =================================================================================================

} // namespace nghttp2_corosio
