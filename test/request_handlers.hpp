#pragma once

//
// Test-only helper coroutines, ported from anyhttp's request_handlers.hpp
// (https://github.com/pgit/anyhttp/blob/master/include/anyhttp/request_handlers.hpp) to
// nghttp2-corosio's Session/Writer/ClientResponse API. Kept under test/ rather than include/,
// since nothing outside the test binary needs them (see server_main.cpp's own local echo()).
//
// Unlike anyhttp's originals (boost::asio awaitable<>, throwing on error), these are io_task<>
// based -- consistent with the rest of this library, and so they can be combined directly with
// boost::capy::when_all()/when_any() (needed for tests where the request body is larger than the
// HTTP/2 flow-control window: the client must read the response concurrently with writing the
// request, or both sides deadlock).
//

#include <nghttp2-corosio/session.hpp>

#include <boost/capy/buffers/make_buffer.hpp>
#include <boost/capy/io_task.hpp>
#include <boost/capy/task.hpp>
#include <boost/corosio/delay.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <string>

namespace nghttp2_corosio_test
{

using Session = nghttp2_corosio::Session;

// =================================================================================================
// Server-side request handlers, registered by path in the ClientAsync fixture (see
// test_client_async.cpp).

/// Streams the request body straight back on the response.
boost::capy::task<> echo(Session::Request request, Session::Response response);

/// Reads and discards the entire request body, then responds with an empty 200.
boost::capy::task<> eat_request(Session::Request request, Session::Response response);

/// Responds 404 with an empty body, without reading the request.
boost::capy::task<> not_found(Session::Response response);

// =================================================================================================
// Misc

/// Suspends for `duration`, or until cancelled.
boost::capy::io_task<> sleep(std::chrono::nanoseconds duration);

/// Gives other coroutines on the same executor a chance to run, `count` times in a row. There's
/// no bare "post via executor" awaitable in capy yet, so this rides a minimal delay() instead --
/// see boost::corosio::delay().
boost::capy::io_task<> yield(std::size_t count = 1);

// =================================================================================================
// Client-side helpers, used by the ClientAsync tests to drive a Session::ClientRequest /
// Session::ClientResponse pair.

/// Writes `bytes` zero-based, incrementing byte values, then closes the request body.
boost::capy::io_task<> send(Session::ClientRequest& request, std::size_t bytes);

/// Reads the entire response body into a string.
boost::capy::io_task<std::string> read(Session::ClientResponse& response);

/// Reads and discards the entire response body, returning its size.
boost::capy::io_task<std::size_t> count(Session::ClientResponse& response);

/// Like count(), but reports the running total via `received` as it accumulates, rather than only
/// in the return value -- for racing a *composite* awaitable (e.g. a when_all()) against
/// corosio::timeout(): when the deadline wins, timeout() discards the wrapped awaitable's actual
/// result wholesale and substitutes a default-initialized payload (see corosio's
/// timeout_awaitable.hpp, await_resume() -- the cancellation it propagates into when_all's
/// children surfaces as cond::canceled on the *combined* result, which is exactly the condition
/// that triggers the substitution), so the byte count would otherwise read back as 0 regardless of
/// how much was actually transferred.
boost::capy::io_task<> count_into(Session::ClientResponse& response, std::size_t& received);

// -------------------------------------------------------------------------------------------------
// Overloads taking a ClientRequest directly: get_response() first, then delegate to the
// ClientResponse overload above. Lets a caller race a response against an in-flight write (see
// PostRange's doc comment in test_client_async.cpp) without a separate get_response() call of its
// own -- get_response() can be awaited concurrently with the write regardless of which coroutine
// actually does the awaiting.

boost::capy::io_task<std::string> read(Session::ClientRequest& request);
boost::capy::io_task<std::size_t> count(Session::ClientRequest& request);
boost::capy::io_task<> count_into(Session::ClientRequest& request, std::size_t& received);

// -------------------------------------------------------------------------------------------------
// Range support: send() overloads for arbitrary byte ranges, contiguous or not (e.g.
// `std::views::iota(uint8_t(0)) | std::views::take(n)`, which is not contiguous).

template <typename Range>
concept ByteRange = std::ranges::range<Range> && (sizeof(std::ranges::range_value_t<Range>) == 1);

/// A contiguous byte range can be handed to the writer directly, without copying.
template <ByteRange Range>
   requires std::ranges::contiguous_range<Range>
boost::capy::io_task<> send(Session::ClientRequest& request, Range range)
{
   auto [ec, written] = co_await request.write(
      boost::capy::make_buffer(std::ranges::data(range), std::ranges::size(range)));
   co_return {ec};
}

/// A non-contiguous byte range (e.g. a `std::views::iota` generator) has to be copied into a
/// buffer first, chunk by chunk.
template <ByteRange Range>
   requires(!std::ranges::contiguous_range<Range>)
boost::capy::io_task<> send(Session::ClientRequest& request, Range range)
{
   std::array<std::uint8_t, 16 * 1024> buffer;
   auto it = std::ranges::begin(range);
   auto last = std::ranges::end(range);
   while (it != last)
   {
      std::size_t n = 0;
      for (; n < buffer.size() && it != last; ++it)
         buffer[n++] = static_cast<std::uint8_t>(*it);

      if (auto [ec, written] = co_await request.write(boost::capy::make_buffer(buffer, n)); ec)
         co_return {ec};
   }
   co_return {};
}

/// Like the non-contiguous send() above, but reports the running total of bytes actually written
/// via `sent` as it goes, for the same reason as count_into(): send() itself never reports a byte
/// count at all (only success/failure), and even if it did, the return value wouldn't survive
/// being raced against corosio::timeout() (see count_into()'s doc comment).
template <ByteRange Range>
   requires(!std::ranges::contiguous_range<Range>)
boost::capy::io_task<> send_into(Session::ClientRequest& request, Range range, std::size_t& sent)
{
   std::array<std::uint8_t, 16 * 1024> buffer;
   auto it = std::ranges::begin(range);
   auto last = std::ranges::end(range);
   while (it != last)
   {
      std::size_t n = 0;
      for (; n < buffer.size() && it != last; ++it)
         buffer[n++] = static_cast<std::uint8_t>(*it);

      auto [ec, written] = co_await request.write(boost::capy::make_buffer(buffer, n));
      sent += written;
      if (ec)
         co_return {ec};
   }
   co_return {};
}

/// Sends `range`, then force-closes the request body regardless of whether the send completed
/// (e.g. after cancellation) -- mirrors anyhttp's sendAndForceEOF().
template <ByteRange Range>
boost::capy::io_task<> sendAndForceEOF(Session::ClientRequest& request, Range range)
{
   auto [ec] = co_await send(request, std::move(range));
   auto [eof_ec] = co_await request.write_eof();
   co_return {ec ? ec : eof_ec};
}

} // namespace nghttp2_corosio_test
