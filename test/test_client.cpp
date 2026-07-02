#include "utils.hpp"

#include <nghttp2-corosio/client.hpp>
#include <nghttp2-corosio/server.hpp>

#include <boost/capy/buffers/make_buffer.hpp>
#include <boost/capy/buffers/string_dynamic_buffer.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/read.hpp>
#include <boost/corosio/ipv4_address.hpp>

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <string>
#include <string_view>
#include <thread>

namespace
{

// Same shape as server_main.cpp's echo handler: streams the request body straight back.
boost::capy::task<> echo(nghttp2_corosio::Session::Request request, nghttp2_corosio::Session::Writer response)
{
   std::array<std::uint8_t, 64 * 1024> buffer;
   for (;;)
   {
      auto [rec, n] =
         co_await request.read_some(boost::capy::mutable_buffer(buffer.data(), buffer.size()));
      if (rec)
         break;

      auto [wec, wn] = co_await response.write(boost::capy::const_buffer(buffer.data(), n));
      if (wec)
         break;
   }

   [[maybe_unused]] auto result = co_await response.write_eof();
}

} // namespace

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

   std::thread([server] { nghttp2_corosio_test::run(server->get_executor().context()); }).detach();

   // Give both sides -- the client's connect and its subsequent session, and the server's accept
   // and its own session -- a moment to run the handshake to completion.
   for (int i = 0; i < 50 && !connected; ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(20));

   EXPECT_TRUE(connected);
}

TEST(ClientTest, EchoesRequestBody)
{
   nghttp2_corosio::Config config;
   config.port = 0; // ask the OS for an unused port
   config.handler = echo;
   auto* server = new nghttp2_corosio::Server(config);
   auto port = server->local_endpoint().port();

   constexpr std::string_view payload = "hello from the client!";
   std::string echoed;
   bool ok = false;
   std::atomic<bool> done = false;

   boost::capy::run_async(server->get_executor())(
      [&, server]() -> boost::capy::task<>
      {
         auto ep = boost::corosio::endpoint(boost::corosio::ipv4_address("127.0.0.1"), port);
         nghttp2_corosio::Client client(server->get_executor());
         auto [ec, session] = co_await client.connect(ep);
         if (ec)
         {
            done = true;
            co_return;
         }

         auto [sec, writer, reader] = co_await session.submit_request("/echo");
         if (sec)
         {
            done = true;
            co_return;
         }

         auto [wec, wn] = co_await writer.write_eof(boost::capy::make_buffer(payload));
         auto [rec, rn] = co_await boost::capy::read(reader, boost::capy::dynamic_buffer(echoed));
         ok = !wec && !rec;
         done = true;
      }());

   std::thread([server] { nghttp2_corosio_test::run(server->get_executor().context()); }).detach();

   for (int i = 0; i < 100 && !done; ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(20));

   ASSERT_TRUE(done);
   EXPECT_TRUE(ok);
   EXPECT_EQ(echoed, payload);
}

} // namespace
