#include <nghttp2-corosio/logging.hpp>
#include <nghttp2-corosio/server.hpp>

#include <boost/capy/buffers.hpp>

#include <array>
#include <cstdlib>
#include <print>
#include <stdexcept>
#include <string_view>

namespace
{

// Streaming echo, the same shape as anyhttp's request_handlers.cpp::echo(): read a chunk, write it
// straight back, repeat until the request body ends. Registered for every path (there's no routing
// yet), but meant to be hit at /echo.
boost::capy::task<> echo(nghttp2_corosio::Session::Request request, nghttp2_corosio::Session::Writer response)
{
   logd("[{}] echo: streaming request body back", request.path());

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

nghttp2_corosio::LogLevel parse_log_level(std::string_view name)
{
   if (name == "debug")
      return nghttp2_corosio::LogLevel::debug;
   if (name == "info")
      return nghttp2_corosio::LogLevel::info;
   if (name == "warn")
      return nghttp2_corosio::LogLevel::warn;
   if (name == "error")
      return nghttp2_corosio::LogLevel::error;
   if (name == "off")
      return nghttp2_corosio::LogLevel::off;
   throw std::invalid_argument(std::format("unknown log level '{}' (want debug/info/warn/error/off)", name));
}

} // namespace

int main(int argc, char* argv[])
{
   nghttp2_corosio::Config config;
   if (argc > 1)
      config.port = static_cast<std::uint16_t>(std::atoi(argv[1]));
   if (argc > 2)
      nghttp2_corosio::set_log_level(parse_log_level(argv[2]));
   config.handler = echo;

   nghttp2_corosio::Server server(config);
   std::println("Try: curl --http2-prior-knowledge --data-binary @somefile http://localhost:{}/echo",
                server.local_endpoint().port());
   server.run();
}
