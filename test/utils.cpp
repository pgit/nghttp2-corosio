#include "utils.hpp"

#include <nghttp2-corosio/logging.hpp>

#include <chrono> // IWYU pragma: keep
#include <cstdlib>
#include <print> // IWYU pragma: keep

namespace nghttp2_corosio_test
{

namespace
{

// The test binary links GTest::gtest_main (stock main(), no hook to run our own setup code before
// RUN_ALL_TESTS), so NGHTTP2_COROSIO_LOG_LEVEL is applied here instead, via a global constructor --
// guaranteed to run before main() regardless. Example: NGHTTP2_COROSIO_LOG_LEVEL=debug
// ./build/test/nghttp2-corosio-tests (or ctest --test-dir build -R <name> --extra-verbose with the
// env var exported first). Leaves the default (info) alone if unset.
struct LogLevelFromEnv
{
   LogLevelFromEnv()
   {
      if (const char* level = std::getenv("NGHTTP2_COROSIO_LOG_LEVEL"))
         nghttp2_corosio::set_log_level(nghttp2_corosio::parse_log_level(level));
   }
} log_level_from_env;

} // namespace

std::size_t run(boost::corosio::io_context& context)
{
#if defined(NDEBUG)
   return context.run();
#else
   std::size_t i = 0;
   using namespace std::chrono;
   auto t0 = steady_clock::now();
   for (i = 0; context.run_one(); ++i)
   {
      auto t1 = steady_clock::now();
      auto dt = duration_cast<milliseconds>(t1 - t0);
      t0 = t1;
      if (dt < 10ms)
         std::println(
            "--- {} ------------------------------------------------------------------------", i);
      else
         std::println("\x1b[1;31m--- {} ({}) "
                      "----------------------------------------------------------------\x1b[0m",
                      i, dt);
   }
   return i;
#endif
}

} // namespace nghttp2_corosio_test
