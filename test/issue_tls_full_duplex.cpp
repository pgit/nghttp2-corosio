// "TLS: concurrent read and write deadlocks" -- co_await capy::when_all(reader, writer) on a
// single openssl_stream never completed both sides: read_input() (invoked when SSL_read needs
// more ciphertext) held corosio's io_cm_ mutex across a blocking socket-level read, so
// flush_output() could never acquire it to send an already-ready reply. Fixed upstream by corosio
// #331; kept here as a regression test.

#include <boost/capy/buffers/make_buffer.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/when_all.hpp>
#include <boost/capy/write.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/corosio/ipv4_address.hpp>
#include <boost/corosio/openssl_stream.hpp>
#include <boost/corosio/tcp_acceptor.hpp>
#include <boost/corosio/tcp_socket.hpp>

#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace capy = boost::capy;
namespace corosio = boost::corosio;

namespace
{

corosio::tls_context make_server_tls_context()
{
   corosio::tls_context ctx;
   std::ignore = ctx.use_certificate_chain_file("test/pki/out/server-fullchain.pem");
   std::ignore = ctx.use_private_key_file("test/pki/out/server-key.pem", corosio::tls_file_format::pem);
   return ctx;
}

corosio::tls_context make_client_tls_context()
{
   corosio::tls_context ctx;
   std::ignore = ctx.load_verify_file("test/pki/out/root.pem");
   std::ignore = ctx.set_verify_mode(corosio::tls_verify_mode::peer);
   return ctx;
}

capy::io_task<> reader(corosio::openssl_stream& stream, std::string& received)
{
   char buf[4096];
   for (;;)
   {
      auto [ec, n] = co_await stream.read_some(capy::make_buffer(buf));
      received.append(buf, n);
      if (ec)
         co_return capy::io_result<>{ec};
   }
}

capy::io_task<> writer(corosio::openssl_stream& stream, std::string_view reply)
{
   auto [ec, n] = co_await capy::write(stream, capy::make_buffer(reply));
   co_return capy::io_result<>{ec};
}

capy::task<> server_session(corosio::tcp_socket peer, corosio::tls_context const& ctx,
                             std::string& received, bool& handshake_ok)
{
   corosio::openssl_stream stream(std::move(peer), ctx);
   if (auto [ec] = co_await stream.handshake(corosio::tls_role::server); ec)
      co_return;
   handshake_ok = true;

   // This is the deadlock from corosio #330: neither side of this when_all() used to complete.
   std::ignore =
      co_await capy::when_all(reader(stream, received), writer(stream, "response payload"));
}

capy::task<> accept_loop(corosio::tcp_acceptor& acceptor, corosio::io_context& ioc,
                          corosio::tls_context const& ctx, std::string& received,
                          bool& handshake_ok)
{
   corosio::tcp_socket peer(ioc);
   if (auto [ec] = co_await acceptor.accept(peer); ec)
      co_return;

   co_await server_session(std::move(peer), ctx, received, handshake_ok);
}

capy::task<> client_session(corosio::io_context& ioc, corosio::endpoint ep,
                             corosio::tls_context const& ctx, std::string& reply, bool& ok)
{
   corosio::tcp_socket socket(ioc);
   if (auto [ec] = co_await socket.connect(ep); ec)
      co_return;

   corosio::openssl_stream stream(std::move(socket), ctx);
   if (auto [ec] = co_await stream.handshake(corosio::tls_role::client); ec)
      co_return;

   auto [wec, wn] = co_await capy::write(stream, capy::make_buffer(std::string_view("request")));
   if (wec)
      co_return;

   char buf[4096];
   auto [rec, rn] = co_await stream.read_some(capy::make_buffer(buf));
   reply.append(buf, rn);
   ok = true;
}

} // namespace

// https://github.com/cppalliance/corosio/issues/330
TEST(Issue, ConcurrentReadAndWriteBothComplete)
{
   corosio::io_context ioc;

   corosio::tcp_acceptor acceptor(ioc);
   acceptor.open();
   ASSERT_FALSE(acceptor.bind(corosio::endpoint(corosio::ipv4_address("127.0.0.1"), 0)));
   ASSERT_FALSE(acceptor.listen());
   auto ep = acceptor.local_endpoint();

   auto server_ctx = make_server_tls_context();
   auto client_ctx = make_client_tls_context();

   std::string server_received;
   std::string client_reply;
   bool server_handshake_ok = false;
   bool client_ok = false;

   capy::run_async(ioc.get_executor())(
      accept_loop(acceptor, ioc, server_ctx, server_received, server_handshake_ok));
   capy::run_async(ioc.get_executor())(
      client_session(ioc, ep, client_ctx, client_reply, client_ok));

   ioc.run();

   EXPECT_TRUE(server_handshake_ok);
   EXPECT_EQ(server_received, "request");
   EXPECT_TRUE(client_ok);
   EXPECT_EQ(client_reply, "response payload");
}
