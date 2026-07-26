#include "request_handlers.hpp"

#include <boost/capy/buffers.hpp>
#include <boost/capy/cond.hpp>

#include <ranges>
#include <utility>

namespace capy = boost::capy;
namespace corosio = boost::corosio;

namespace nghttp2_corosio_test
{

// =================================================================================================

// submit() is deferred to just before the first write rather than called up front -- see the
// comment on server_main.cpp's echo() (same shape) for why that matters for throughput.
capy::task<> echo(Session::Request request, Session::Response response)
{
   bool submitted = false;
   std::array<std::uint8_t, 64 * 1024> buffer;
   for (;;)
   {
      auto [rec, n] = co_await request.read_some(capy::make_buffer(buffer));

      if (!std::exchange(submitted, true))
         [[maybe_unused]] auto s = co_await response.submit();

      auto [wec, wn] = co_await response.write(capy::make_buffer(buffer, n));
      if (rec || wec)
         break;
   }
   if (!submitted)
      [[maybe_unused]] auto s = co_await response.submit();

   [[maybe_unused]] auto result = co_await response.write_eof();
}

capy::task<> eat_request(Session::Request request, Session::Response response)
{
   // Submit the (empty, 200) response right away, before the request body has been fully
   // consumed -- lets a client observe the response concurrently with sending its request.
   [[maybe_unused]] auto submitted = co_await response.submit();

   std::array<std::uint8_t, 16 * 1024> buffer;
   for (;;)
   {
      auto [ec, n] = co_await request.read_some(capy::make_buffer(buffer));
      if (ec)
         break;
   }

   [[maybe_unused]] auto result = co_await response.write_eof();
}

capy::task<> not_found(Session::Response response)
{
   [[maybe_unused]] auto submitted = co_await response.submit(404);
   [[maybe_unused]] auto result = co_await response.write_eof();
}

// =================================================================================================

capy::io_task<> sleep(std::chrono::nanoseconds duration)
{
   co_return co_await corosio::delay(duration);
}

capy::io_task<> yield(std::size_t count)
{
   for (std::size_t i = 0; i < count; ++i)
      if (auto [ec] = co_await corosio::delay(std::chrono::microseconds(1)); ec)
         co_return {ec};
   co_return {};
}

// =================================================================================================

capy::io_task<> send(Session::ClientRequest& request, std::size_t bytes)
{
   co_return co_await sendAndForceEOF(request,
                                      std::views::iota(std::uint8_t{0}) | std::views::take(bytes));
}

capy::io_task<std::string> read(Session::ClientResponse& response)
{
   std::string body;
   std::array<char, 1024> buffer;
   for (;;)
   {
      auto [ec, n] = co_await response.read_some(capy::make_buffer(buffer));
      if (n)
         body.append(buffer.data(), n);
      if (ec)
      {
         // Reaching end-of-stream is how this loop is meant to end -- not a failure to report.
         if (ec == capy::cond::eof)
            ec = {};
         co_return {ec, std::move(body)};
      }
   }
}

capy::io_task<std::size_t> count(Session::ClientResponse& response)
{
   std::size_t bytes = 0;
   std::array<std::uint8_t, 16 * 1024> buffer;
   for (;;)
   {
      auto [ec, n] = co_await response.read_some(capy::make_buffer(buffer));
      bytes += n;
      if (ec)
      {
         // Reaching end-of-stream is how this loop is meant to end -- not a failure to report.
         if (ec == capy::cond::eof)
            ec = {};
         co_return {ec, bytes};
      }
   }
}

capy::io_task<> count_into(Session::ClientResponse& response, std::size_t& received)
{
   std::array<std::uint8_t, 16 * 1024> buffer;
   for (;;)
   {
      auto [ec, n] = co_await response.read_some(capy::make_buffer(buffer));
      received += n;
      if (ec)
      {
         // Reaching end-of-stream is how this loop is meant to end -- not a failure to report.
         if (ec == capy::cond::eof)
            ec = {};
         co_return {ec};
      }
   }
}

// -------------------------------------------------------------------------------------------------

capy::io_task<std::string> read(Session::ClientRequest& request)
{
   auto [ec, response] = co_await request.get_response();
   if (ec)
      co_return {ec, {}};
   co_return co_await read(response);
}

capy::io_task<std::size_t> count(Session::ClientRequest& request)
{
   auto [ec, response] = co_await request.get_response();
   if (ec)
      co_return {ec, 0u};
   co_return co_await count(response);
}

capy::io_task<> count_into(Session::ClientRequest& request, std::size_t& received)
{
   auto [ec, response] = co_await request.get_response();
   if (ec)
      co_return {ec};
   co_return co_await count_into(response, received);
}

// =================================================================================================

} // namespace nghttp2_corosio_test
