#include <nghttp2-corosio/client.hpp>
#include <nghttp2-corosio/server.hpp>

#include <boost/capy/ex/run_async.hpp>
#include <boost/corosio/ipv4_address.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

namespace
{

// See the comment in test_server.cpp's ServerTest fixture: server (and, here, the session it
// accepts) are deliberately leaked rather than torn down, to sidestep a known corosio shutdown
// race. Safe in this short-lived test binary.
TEST(ClientTest, ConnectsToServer)
{
   nghttp2_corosio::Config config;
   config.port = 0; // ask the OS for an unused port
   auto* server = new nghttp2_corosio::Server(config);
   auto port = server->local_endpoint().port();

   std::atomic<bool> connected = false;
   boost::capy::run_async(server->get_executor())(
      [&, server]() -> boost::capy::task<>
      {
         auto ep = boost::corosio::endpoint(boost::corosio::ipv4_address("127.0.0.1"), port);
         nghttp2_corosio::Client client(server->get_executor());
         auto [ec, session] = co_await client.connect(ep);
         connected = !ec && session;
      }());

   std::thread([server] { server->run(); }).detach();

   // Give both sides -- the client's connect and its subsequent session, and the server's accept
   // and its own session -- a moment to run the handshake to completion.
   for (int i = 0; i < 50 && !connected; ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(20));

   EXPECT_TRUE(connected);
}

} // namespace
