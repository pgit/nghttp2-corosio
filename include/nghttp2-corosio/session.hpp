#pragma once

#include "nghttp2-corosio/logging.hpp"

#include <boost/capy/buffers.hpp>
#include <boost/capy/ex/any_executor.hpp>
#include <boost/capy/io/any_read_source.hpp>
#include <boost/capy/io/any_write_sink.hpp>
#include <boost/capy/io_task.hpp>

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

   /// An ordered list of header name/value pairs, in submission order. Used both for
   /// Response::set() and (once request headers are captured beyond :path) incoming requests.
   using Headers = std::vector<std::pair<std::string, std::string>>;

   /// The incoming request on a server session: the requested path (the raw :path pseudo-header;
   /// no query/fragment splitting or URL parsing yet) plus the request body. Request forwards
   /// read_some()/read() to the body, so it can be used directly wherever a Reader is expected.
   class Request
   {
   public:
      Request(std::string path, Reader reader) : path_(std::move(path)), reader_(std::move(reader))
      {
         logd("\x1b[1;35mServer::Request: ctor\x1b[0m");
      }

      // reader_ is empty (operator bool() == false) after being moved from, which is what a
      // moved-out husk looks like once it's passed through a chain of by-value handler layers
      // (RequestHandler -> dispatch() -> the actual per-test handler, say) -- skip the log then,
      // matching anyhttp's `if (impl)` guard in its own Request::reset(), so only the one object
      // that's actually still holding the body logs its destruction.
      ~Request()
      {
         if (reader_)
            logd("\x1b[35mServer::Request: dtor\x1b[0m");
      }

      // Reader is move-only; the destructor above suppresses the implicit move members, so they
      // need restating explicitly.
      Request(Request&&) noexcept = default;
      Request& operator=(Request&&) noexcept = default;

      std::string_view path() const noexcept { return path_; }

      template <boost::capy::MutableBufferSequence MB>
      auto read_some(MB buffers)
      {
         return reader_.read_some(buffers);
      }

      template <boost::capy::MutableBufferSequence MB>
      boost::capy::io_task<std::size_t> read(MB buffers)
      {
         return reader_.read(buffers);
      }

   private:
      std::string path_;
      Reader reader_;
   };

   /// The incoming response on a client session: an asynchronously-arriving `:status`
   /// pseudo-header plus the response body. ClientResponse forwards read_some()/read() to the
   /// body, so it can be used directly wherever a Reader is expected.
   class ClientResponse
   {
   public:
      /// Waits for the `:status` pseudo-header to arrive; constructed by Session::Impl, which is
      /// the only thing that knows how to ask the underlying Stream.
      using StatusFn = std::function<boost::capy::io_task<unsigned int>()>;

      ClientResponse(Reader reader, StatusFn status)
         : reader_(std::move(reader)), status_(std::move(status))
      {
      }

      /// Waits for the response's `:status` pseudo-header to arrive. Safe to call more than
      /// once -- returns immediately once the status has already been captured.
      boost::capy::io_task<unsigned int> status() { return status_(); }

      template <boost::capy::MutableBufferSequence MB>
      auto read_some(MB buffers)
      {
         return reader_.read_some(buffers);
      }

      template <boost::capy::MutableBufferSequence MB>
      boost::capy::io_task<std::size_t> read(MB buffers)
      {
         return reader_.read(buffers);
      }

   private:
      Reader reader_;
      StatusFn status_;
   };

   /// The outgoing response on a server session: a status code and headers, submitted in a single
   /// explicit call, plus the response body. Response forwards write_some()/write()/write_eof() to
   /// the body, so it can be used directly wherever a Writer is expected.
   ///
   /// Unlike a status/headers accumulator submitted lazily on first write, submit() sends the
   /// HEADERS frame right away -- nghttp2 takes the whole header block in one call regardless, so
   /// there's nothing to gain by letting callers dribble it in via setters first. A handler must
   /// call submit() before its first write; write_some()/write()/write_eof() below don't check.
   class Response
   {
   public:
      /// `submit` performs the actual header submission; constructed by Session::Impl, which is
      /// the only thing that knows how to talk to nghttp2.
      using SubmitFn =
         std::function<boost::capy::io_task<>(unsigned int status, const Headers& headers)>;

      Response(Writer writer, SubmitFn submit)
         : writer_(std::move(writer)), submit_(std::move(submit))
      {
         logd("\x1b[1;35mServer::Response: ctor\x1b[0m");
      }

      // See Request::~Request() above -- writer_ goes empty on move-out, so this only fires for
      // the one instance that's actually still holding the body.
      ~Response()
      {
         if (writer_)
            logd("\x1b[35mServer::Response: dtor\x1b[0m");
      }

      // Writer is move-only; the destructor above suppresses the implicit move members, so they
      // need restating explicitly.
      Response(Response&&) noexcept = default;
      Response& operator=(Response&&) noexcept = default;

      /// Submits the response status/headers. Must be called exactly once, before any write.
      boost::capy::io_task<> submit(unsigned int status = 200, Headers headers = {})
      {
         co_return co_await submit_(status, headers);
      }

      template <boost::capy::ConstBufferSequence CB>
      boost::capy::io_task<std::size_t> write_some(CB buffers)
      {
         co_return co_await writer_.write_some(buffers);
      }

      template <boost::capy::ConstBufferSequence CB>
      boost::capy::io_task<std::size_t> write(CB buffers)
      {
         co_return co_await writer_.write(buffers);
      }

      template <boost::capy::ConstBufferSequence CB>
      boost::capy::io_task<std::size_t> write_eof(CB buffers)
      {
         co_return co_await writer_.write_eof(buffers);
      }

      boost::capy::io_task<> write_eof() { co_return co_await writer_.write_eof(); }

   private:
      Writer writer_;
      SubmitFn submit_;
   };

   Session() = default;
   explicit Session(std::shared_ptr<Impl> impl);
   Session(Session&& other) noexcept;
   Session& operator=(Session&& other) noexcept;
   ~Session();

   constexpr operator bool() const noexcept { return static_cast<bool>(impl_); }

   using executor_type = boost::capy::any_executor;
   executor_type get_executor() const noexcept;

   /// Submits a request on a new stream (client sessions only) and returns a Writer for the
   /// request body and a ClientResponse for the response. Pseudo-headers beyond :method/:path
   /// are currently fixed (POST, http, an authority derived from the peer endpoint) -- a real
   /// headers API will come later.
   boost::capy::io_task<Writer, ClientResponse> submit_request(std::string_view path);

private:
   std::shared_ptr<Impl> impl_;
};

// =================================================================================================

} // namespace nghttp2_corosio
