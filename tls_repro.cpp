// Standalone, minimal reproduction of a deadlock in corosio's openssl_stream backend -- plain TLS
// stream reads/writes, no nghttp2 or nghttp2-corosio involved. See test/test_tls.cpp's
// DISABLED_EchoesRequestBodyOverTls for the full write-up of the bug; this file exists to let
// anyone (in particular, a corosio maintainer) reproduce it without building the rest of this
// project.
//
// The shape: after the TLS handshake, the server concurrently (a) reads, expecting more input from
// the client, and (b) writes a reply it already has in hand -- exactly what
// Session::Impl::recv_loop()/send_loop() do for every real HTTP/2 request/response, and already
// correct for plaintext sockets. openssl_stream::impl serializes both directions behind one io_cm_
// async_mutex, and the read acquires it *before* blocking on the underlying socket read; since the
// client has nothing more to send (it's waiting on this very reply), that read never completes, so
// the write can never acquire the mutex to send anything. Circular wait -- this program hangs
// forever instead of printing "server: session done".
//
// Usage: run test/pki/create.sh once first (or build the `test_pki_certs` CMake target), then
//    ./build/tls_repro
// and observe that it never terminates (Ctrl-C or `timeout` to stop it).

#include <boost/capy/buffers/make_buffer.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/when_all.hpp>
#include <boost/capy/write.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/corosio/ipv4_address.hpp>
#include <boost/corosio/openssl_stream.hpp>
#include <boost/corosio/tcp_acceptor.hpp>
#include <boost/corosio/tcp_socket.hpp>

#include <cstdio>
#include <print>
#include <string_view>
#include <system_error>

namespace capy = boost::capy;
namespace corosio = boost::corosio;

namespace
{

corosio::tls_context make_server_tls_context()
{
   corosio::tls_context ctx;
   ctx.use_certificate_chain_file("test/pki/out/server-fullchain.pem");
   ctx.use_private_key_file("test/pki/out/server-key.pem", corosio::tls_file_format::pem);
   return ctx;
}

corosio::tls_context make_client_tls_context()
{
   corosio::tls_context ctx;
   ctx.load_verify_file("test/pki/out/root.pem");
   ctx.set_verify_mode(corosio::tls_verify_mode::peer);
   return ctx;
}

// Loops reading until the peer closes or errors -- there is always more to read attempted,
// mirroring recv_loop()'s shape of "keep reading frames for as long as the connection is open".
capy::io_task<> reader(corosio::openssl_stream& stream)
{
   char buf[4096];
   for (;;)
   {
      auto [ec, n] = co_await stream.read_some(capy::make_buffer(buf));
      std::println("server: reader: read {} bytes, ec={}", n, ec.message());
      if (ec)
         co_return capy::io_result<>{ec};
   }
}

capy::io_task<> writer(corosio::openssl_stream& stream, std::string_view reply)
{
   auto [ec, n] = co_await capy::write(stream, capy::make_buffer(reply));
   std::println("server: writer: wrote {} bytes, ec={}", n, ec.message());
   co_return capy::io_result<>{ec};
}

capy::task<> server_session(corosio::tcp_socket peer, corosio::tls_context const& ctx)
{
   corosio::openssl_stream stream(std::move(peer), ctx);

   std::println("server: handshake...");
   if (auto [ec] = co_await stream.handshake(corosio::tls_role::server); ec)
   {
      std::println("server: handshake failed: {}", ec.message());
      co_return;
   }
   std::println("server: handshake done");

   // The deadlock: this concurrent read+write on one openssl_stream never both complete.
#if 1
   std::ignore = co_await capy::when_all(reader(stream), writer(stream, "response payload"));
#else
   std::ignore = co_await writer(stream, "response payload");
   std::ignore = co_await reader(stream);
#endif

   std::println("server: session done");
}

capy::task<> accept_loop(corosio::tcp_acceptor& acceptor, corosio::io_context& ioc,
                         corosio::tls_context const& ctx)
{
   corosio::tcp_socket peer(ioc);
   auto [ec] = co_await acceptor.accept(peer);
   if (ec)
   {
      std::println("server: accept failed: {}", ec.message());
      co_return;
   }

   co_await server_session(std::move(peer), ctx);
}

capy::task<> client_session(corosio::io_context& ioc, corosio::endpoint ep,
                            corosio::tls_context const& ctx)
{
   corosio::tcp_socket socket(ioc);
   if (auto [ec] = co_await socket.connect(ep); ec)
   {
      std::println("client: connect failed: {}", ec.message());
      co_return;
   }

   corosio::openssl_stream stream(std::move(socket), ctx);
   std::println("client: handshake...");
   if (auto [ec] = co_await stream.handshake(corosio::tls_role::client); ec)
   {
      std::println("client: handshake failed: {}", ec.message());
      co_return;
   }
   std::println("client: handshake done");

   auto [wec, wn] = co_await capy::write(stream, capy::make_buffer(std::string_view("request")));
   std::println("client: wrote {} bytes, ec={}", wn, wec.message());

   // This is where it hangs: the server's reply is stuck behind the deadlock above.
   char buf[4096];
   std::println("client: reading reply...");
   auto [rec, rn] = co_await stream.read_some(capy::make_buffer(buf));
   std::println("client: read {} bytes, ec={}", rn, rec.message());
}

} // namespace

int main()
{
   // Unbuffered so progress is visible even when killed mid-hang (e.g. under `timeout`) with
   // stdout redirected to a file/pipe rather than a tty.
   std::setvbuf(stdout, nullptr, _IONBF, 0);

   corosio::io_context ioc;

   corosio::tcp_acceptor acceptor(ioc);
   acceptor.open();
   if (auto ec = acceptor.bind(corosio::endpoint(corosio::ipv4_address("127.0.0.1"), 0)); ec)
      throw std::system_error(ec);
   if (auto ec = acceptor.listen(); ec)
      throw std::system_error(ec);
   auto ep = acceptor.local_endpoint();

   auto server_ctx = make_server_tls_context();
   auto client_ctx = make_client_tls_context();

   capy::run_async(ioc.get_executor())(accept_loop(acceptor, ioc, server_ctx));
   capy::run_async(ioc.get_executor())(client_session(ioc, ep, client_ctx));

   std::println("running -- this hangs on the corosio deadlock instead of reaching the end");
   ioc.run();
   std::println("done");

   return 0;
}
