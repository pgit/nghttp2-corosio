#pragma once

#include <boost/corosio/io_context.hpp>

#include <cstddef>

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

} // namespace nghttp2_corosio_test
