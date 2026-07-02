#include <boost/capy.hpp>
#include <boost/corosio.hpp>
#include <iostream>

namespace corosio = boost::corosio;
namespace capy = boost::capy;

capy::task<> echo_session(corosio::tcp_socket socket)
{
   char buf[1024];
   std::size_t total_bytes = 0;

   for (;;)
   {
      auto [ec, n] = co_await socket.read_some(capy::mutable_buffer(buf, sizeof(buf)));
      if (ec)
         break;

      auto [wec, wn] = co_await capy::write(socket, capy::const_buffer(buf, n));
      if (wec)
         break;

      total_bytes += static_cast<std::size_t>(wn);
   }

   std::cout << "Session ended, echoed " << total_bytes << " bytes\n";
}

capy::task<> accept_loop(corosio::tcp_acceptor& acc, corosio::io_context& ioc)
{
   auto ep = acc.local_endpoint();
   std::cout << "Listening on port " << ep.port() << "\n";

   for (;;)
   {
      corosio::tcp_socket peer(ioc);
      auto [ec] = co_await acc.accept(peer);

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

      capy::run_async(ioc.get_executor())(echo_session(std::move(peer)));
   }
}

int main(int argc, char* argv[])
{
   unsigned short port = 8080;
   if (argc > 1)
      port = static_cast<unsigned short>(std::atoi(argv[1]));

   corosio::io_context context;
   corosio::tcp_acceptor acceptor(context);
   acceptor.open();
   acceptor.set_option(corosio::socket_option::reuse_address(true));
   if (auto ec = acceptor.bind(corosio::endpoint(port)))
      throw std::system_error(ec);
   if (auto ec = acceptor.listen())
      throw std::system_error(ec);

   capy::run_async(context.get_executor())(accept_loop(acceptor, context));

   context.run();

   return 0;
}
