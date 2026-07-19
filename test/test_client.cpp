#include "boost/capy/ex/this_coro.hpp"
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
#include <string>
#include <string_view>

namespace
{

// Same shape as server_main.cpp's echo handler: streams the request body straight back.
boost::capy::task<> echo(nghttp2_corosio::Session::Request request,
                         nghttp2_corosio::Session::Response response)
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

// Free coroutines rather than capturing lambdas: a capturing lambda that is a coroutine and gets
// invoked as a temporary (`[...]() -> task<>{...}()`) dangles as soon as the coroutine suspends
// past the statement's end -- captures live in the closure object, and the coroutine frame only
// keeps a pointer back to it. Plain functions have no such temporary to outlive.
boost::capy::task<> connect_to_server(nghttp2_corosio::Server::executor_type executor,
                                      std::uint16_t port, bool& connected)
{
   auto ep = boost::corosio::endpoint(boost::corosio::ipv4_address("127.0.0.1"), port);
   nghttp2_corosio::Client client(executor);
   auto [ec, session] = co_await client.connect(ep);
   connected = !ec && session;
}

boost::capy::task<> echo_request(std::uint16_t port, std::string_view payload, std::string& echoed,
                                 bool& ok)
{
   auto ex = co_await boost::capy::this_coro::executor;
   auto ep = boost::corosio::endpoint(boost::corosio::ipv4_address("127.0.0.1"), port);
   nghttp2_corosio::Client client(ex);
   auto [ec, session] = co_await client.connect(ep);
   if (ec)
      co_return;

   auto [sec, writer, reader] = co_await session.submit_request("/echo");
   if (sec)
      co_return;

   auto [wec, wn] = co_await writer.write_eof(boost::capy::make_buffer(payload));
   auto [rec, rn] = co_await boost::capy::read(reader, boost::capy::dynamic_buffer(echoed));
   ok = !wec && !rec;
}

} // namespace

namespace
{

// server is a plain stack-local value, destroyed exactly once (at the closing brace below) --
// after nghttp2_corosio_test::run() has returned control here, never while anything is still
// executing inside its io_context. ~Server() cancels and joins the accept loop, every session it
// accepted, and the Client's own session (registered into the same per-context TaskGroup -- see
// task_group.hpp), all synchronously, before its io_context is torn down.
TEST(ClientTest, ConnectsToServer)
{
   nghttp2_corosio::Config config;
   config.port = 0; // ask the OS for an unused port
   nghttp2_corosio::Server server(config);
   auto port = server.local_endpoint().port();

   // No thread is spawned: the completion handler stops the server, which is what makes the
   // synchronous run() below (driving the server's own io_context on this thread) return.
   bool connected = false;
   boost::capy::run_async(server.get_executor(), [&server] { server.stop(); },
                          [&server](std::exception_ptr) { server.stop(); })(
      connect_to_server(server.get_executor(), port, connected));

   nghttp2_corosio_test::run(server.get_executor().context());

   EXPECT_TRUE(connected);
}

TEST(ClientTest, EchoesRequestBody)
{
   nghttp2_corosio::Config config;
   config.port = 0; // ask the OS for an unused port
   config.handler = echo;
   nghttp2_corosio::Server server(config);
   auto port = server.local_endpoint().port();

   constexpr std::string_view payload = "hello from the client!";
   std::string echoed;
   bool ok = false;

   boost::capy::run_async(server.get_executor(), [&server] { server.stop(); },
                          [&server](std::exception_ptr) { server.stop(); })(
      echo_request(port, payload, echoed, ok));

   nghttp2_corosio_test::run(server.get_executor().context());

   EXPECT_TRUE(ok);
   EXPECT_EQ(echoed, payload);
}

} // namespace
