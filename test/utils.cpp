#include "utils.hpp"

#include <nghttp2-corosio/logging.hpp>
#include <nghttp2-corosio/run_io_context.hpp>

#include <cstdlib>

namespace corosio = boost::corosio;

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

std::size_t run(corosio::io_context& context) { return nghttp2_corosio::run_io_context(context); }

} // namespace nghttp2_corosio_test
