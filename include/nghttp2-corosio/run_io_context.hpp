#pragma once

#include "logging.hpp"

#include <boost/corosio/io_context.hpp>

#include <chrono>
#include <cstddef>
#include <print> // IWYU pragma: keep

namespace nghttp2_corosio
{

// =================================================================================================
//
// Like io_context::run(), but at debug log level steps through run_one() one handler at a time and
// prints per-iteration timing -- lets callers observe what happens at each IO event, which is handy
// when chasing scheduler edge cases.
//
// =================================================================================================

inline std::size_t run_io_context(boost::corosio::io_context& context)
{
   if (log_level() > LogLevel::debug)
      return context.run();

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
}

} // namespace nghttp2_corosio
