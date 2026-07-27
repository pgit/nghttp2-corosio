#include "echo_handler.hpp"

#include <nghttp2-corosio/logging.hpp>
#include <nghttp2-corosio/server.hpp>

#include <cstdlib>
#include <print>

int main(int argc, char* argv[])
{
   nghttp2_corosio::Config config;
   if (argc > 1)
      config.port = static_cast<std::uint16_t>(std::atoi(argv[1]));
   if (argc > 2)
      nghttp2_corosio::set_log_level(nghttp2_corosio::parse_log_level(argv[2]));
   config.handler = echo_handler;

   nghttp2_corosio::Server server(config);
   std::println(
      "Try: curl --http2-prior-knowledge --data-binary @somefile http://localhost:{}/echo",
      server.local_endpoint().port());

   server.run();
}
