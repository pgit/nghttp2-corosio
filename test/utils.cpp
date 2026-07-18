#include "utils.hpp"

#include <nghttp2-corosio/server.hpp>

#include <chrono> // IWYU pragma: keep
#include <print> // IWYU pragma: keep
#include <vector>

namespace nghttp2_corosio_test
{

nghttp2_corosio::Server* leak(nghttp2_corosio::Server* server)
{
   // `leaked` must itself never be destroyed: a plain function-local `static vector` runs its
   // destructor during normal exit()-time static teardown, which happens *before* valgrind's
   // final leak scan -- freeing the vector's backing storage (and with it, the only remaining
   // pointer to `server`) right before that scan runs, so it shows up as "definitely lost"
   // despite having been "kept reachable" the whole time the process was actually running.
   // Leaking the vector itself (via `new`, never `delete`d) means nothing ever frees that
   // backing storage, so it's still there -- and still reachable -- when the scan happens.
   static auto* leaked = new std::vector<nghttp2_corosio::Server*>();
   leaked->push_back(server);
   return server;
}

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
