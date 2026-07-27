#include "echo_handler.hpp"

#include <nghttp2-corosio/logging.hpp>

#include <boost/capy/buffers/make_buffer.hpp>

#include <array>
#include <cstdint>

namespace capy = boost::capy;
using nghttp2_corosio::Session;

capy::io_task<> echo(Session::Request request, Session::Response response)
{
   // The echoed body is byte-for-byte what was read, so the request's content-length (if any)
   // still applies to the response -- set before the first submit() below, per submit()'s doc
   // comment in session.hpp.
   response.content_length(request.content_length());

   bool submitted = false;
   if (request.has_header("x-eager-submit"))
   {
      if (auto r = co_await response.submit(); r.ec)
         co_return r.ec;
      submitted = true;
   }

   std::array<std::uint8_t, 64 * 1024> buffer;
   for (;;)
   {
      auto [rec, n] = co_await request.read_some(capy::make_buffer(buffer));
      if (rec)
         break;

      if (!submitted)
      {
         if (auto r = co_await response.submit(); r.ec)
            co_return r.ec;
         submitted = true;
      }

      auto [wec, wn] = co_await response.write(capy::make_buffer(buffer, n));
      if (wec)
         break;
   }

   if (!submitted)
      if (auto r = co_await response.submit(); r.ec)
         co_return r.ec;

   co_return co_await response.write_eof();
}

capy::task<> echo_handler(Session::Request request, Session::Response response)
{
   auto r = co_await echo(std::move(request), std::move(response));
   if (r.ec)
      logw("echo: {}", r.ec.message());
}
