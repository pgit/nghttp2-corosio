#include <nghttp2-corosio/server.hpp>

#include <cstdlib>

int main(int argc, char* argv[])
{
   nghttp2_corosio::Config config;
   if (argc > 1)
      config.port = static_cast<std::uint16_t>(std::atoi(argv[1]));

   nghttp2_corosio::Server server(config);
   server.run();
}
