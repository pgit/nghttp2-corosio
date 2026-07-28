#pragma once

#include <nghttp2-corosio/session.hpp>

#include <boost/capy/io_task.hpp>
#include <boost/capy/task.hpp>

// =================================================================================================
//
// Streaming echo: reads a chunk of the request body, writes it straight back onto the response,
// repeats until the request body ends. The single implementation of that loop -- shared by
// server_main.cpp and the test suite (test/request_handlers.cpp, test/test_client.cpp) rather than
// each keeping its own copy.
//
// submit() is deliberately deferred to just before the first write rather than called up front:
// submitting before any body bytes are available forces nghttp2 to flush a standalone HEADERS
// frame and defer the data provider (NGHTTP2_ERR_DEFERRED) until the first write() resumes it --
// an extra round trip through Session::Impl::send_loop()'s wait/wake cycle on every request.
// Measured ~40% lower h2load throughput (`-n 10000 -m 4 -c 3`, 64KiB request/response bodies) than
// submitting once the first chunk is already in hand, where nghttp2 can fold the HEADERS and first
// DATA frame into one send_loop() pass.
//
// A request carrying an "x-eager-submit" header opts into that up-front, eager submit() instead --
// lets a test deliberately exercise the deferred-provider path rather than the throughput-optimized
// default.
//
// =================================================================================================

/// The echo loop itself. Reports failure via the returned io_result rather than throwing or
/// swallowing it -- see echo_handler() below for the task<>-returning adapter most callers want.
boost::capy::io_task<> echo(nghttp2_corosio::Session::Request request,
                            nghttp2_corosio::Session::Response response);

/// Adapts echo() to the task<>-returning signature nghttp2_corosio::Config::handler needs, logging
/// (rather than propagating) any error echo() itself reports.
boost::capy::task<> echo_handler(nghttp2_corosio::Session::Request request,
                                 nghttp2_corosio::Session::Response response);
