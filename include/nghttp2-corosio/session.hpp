#pragma once

#include <boost/capy/ex/any_executor.hpp>
#include <boost/capy/io/any_read_source.hpp>
#include <boost/capy/io/any_write_sink.hpp>
#include <boost/capy/io_task.hpp>

#include <memory>
#include <string_view>

namespace nghttp2_corosio
{

// =================================================================================================

class Session
{
public:
   class Impl;

   /// A readable body: the incoming request on a server session, or the incoming response on a
   /// client session. Any concrete type satisfying capy's ReadSource concept can be type-erased
   /// into one of these -- see https://develop.capy.cpp.al/capy/6.streams/6.intro.html.
   using Reader = boost::capy::any_read_source;

   /// A writable body: the outgoing response on a server session, or the outgoing request on a
   /// client session. Any concrete type satisfying capy's WriteSink concept can be type-erased
   /// into one of these.
   using Writer = boost::capy::any_write_sink;

   Session() = default;
   explicit Session(std::shared_ptr<Impl> impl);
   Session(Session&& other) noexcept;
   Session& operator=(Session&& other) noexcept;
   ~Session();

   constexpr operator bool() const noexcept { return static_cast<bool>(impl_); }

   using executor_type = boost::capy::any_executor;
   executor_type get_executor() const noexcept;

   /// Submits a request on a new stream (client sessions only) and returns a Writer for the
   /// request body and a Reader for the response body. Pseudo-headers beyond :method/:path are
   /// currently fixed (POST, http, an authority derived from the peer endpoint) -- a real headers
   /// API will come later.
   boost::capy::io_task<Writer, Reader> submit_request(std::string_view path);

private:
   std::shared_ptr<Impl> impl_;
};

// =================================================================================================

} // namespace nghttp2_corosio
