#include <nghttp2-corosio/logging.hpp>
#include <nghttp2-corosio/server.hpp>

#include <boost/capy/buffers.hpp>
#include <boost/capy/buffers/make_buffer.hpp>

#include <array>
#include <cstdlib>
#include <print>
#include <utility>

namespace capy = boost::capy;

namespace
{

// Streaming echo, the same shape as anyhttp's request_handlers.cpp::echo(): read a chunk, write it
// straight back, repeat until the request body ends. Registered for every path (there's no routing
// yet), but meant to be hit at /echo.
//
// submit() is deliberately deferred to just before the first write rather than called up front:
// submitting before any body bytes are available forces nghttp2 to flush a standalone HEADERS
// frame and defer the data provider (NGHTTP2_ERR_DEFERRED) until the first write() resumes it --
// an extra round trip through Session::Impl::send_loop()'s wait/wake cycle on every request.
// Measured ~40% lower h2load throughput (`-n 10000 -m 4 -c 3`, 64KiB request/response bodies) than
// submitting once the first chunk is already in hand, where nghttp2 can fold the HEADERS and first
// DATA frame into one send_loop() pass.
capy::task<> echo(nghttp2_corosio::Session::Request request,
                         nghttp2_corosio::Session::Response response)
{
   logd("[{}] echo: streaming request body back", request.path());
   static const nghttp2_corosio::Session::Headers headers{{"content-type", "application/octet-stream"}};

   // The echoed body is byte-for-byte what was read, so the request's content-length (if any)
   // still applies to the response -- set before the first submit() below, per submit()'s doc
   // comment in session.hpp.
   response.content_length(request.content_length());

   bool submitted = false;
   std::array<std::uint8_t, 64 * 1024> buffer;
   for (;;)
   {
      auto [rec, n] = co_await request.read_some(capy::make_buffer(buffer));
      if (rec)
         break;

      if (!std::exchange(submitted, true))
         [[maybe_unused]] auto s = co_await response.submit(200, headers);

      auto [wec, wn] = co_await response.write(capy::make_buffer(buffer, n));
      if (wec)
         break;
   }
   if (!submitted)
      [[maybe_unused]] auto s = co_await response.submit(200, headers);

   [[maybe_unused]] auto result = co_await response.write_eof();
}

} // namespace

int main(int argc, char* argv[])
{
   nghttp2_corosio::Config config;
   if (argc > 1)
      config.port = static_cast<std::uint16_t>(std::atoi(argv[1]));
   if (argc > 2)
      nghttp2_corosio::set_log_level(nghttp2_corosio::parse_log_level(argv[2]));
   config.handler = echo;

   nghttp2_corosio::Server server(config);
   std::println(
      "Try: curl --http2-prior-knowledge --data-binary @somefile http://localhost:{}/echo",
      server.local_endpoint().port());

   server.run();
}
