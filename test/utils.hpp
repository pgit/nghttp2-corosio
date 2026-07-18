#pragma once

#include <boost/corosio/io_context.hpp>

#include <cstddef>

namespace nghttp2_corosio
{
class Server;
} // namespace nghttp2_corosio

namespace nghttp2_corosio_test
{

// =================================================================================================
//
// Like io_context::run(), but in debug builds steps through run_one() one handler at a time and
// prints per-iteration timing -- lets tests observe what happens at each IO event, which is handy
// when chasing scheduler edge cases (see the "Known issue" in CLAUDE.md).
//
// =================================================================================================

std::size_t run(boost::corosio::io_context& context);

// =================================================================================================
//
// Registers `server` (allocated with `new`) in a process-lifetime container so it stays
// reachable for the rest of the test binary's run, even though nothing ever `delete`s it -- see
// CLAUDE.md's "Known issue" for why tests leak their Server instead of tearing it down (confirmed
// empirically: destroying a Server whose session hasn't fully wound down segfaults/corrupts, not
// just theoretically -- a suspended send_loop() coroutine frame gets force-destroyed along with
// the io_context, double-freeing a std::stop_state/stop_callback in the process). Without this
// helper, a Server leaked from a test fixture becomes unreachable once the fixture object is
// destroyed at the end of the test, and tools like valgrind (see test/CMakeLists.txt's
// EchoLoopValgrind) report it as "definitely lost" rather than the intended, harmless "still
// reachable".
//
// Returns `server` unchanged so callers can leak it inline:
//    server_ = nghttp2_corosio_test::leak(new nghttp2_corosio::Server(config));
//
// =================================================================================================

nghttp2_corosio::Server* leak(nghttp2_corosio::Server* server);

} // namespace nghttp2_corosio_test
