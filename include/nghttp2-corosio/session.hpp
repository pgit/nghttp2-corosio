#pragma once

#include "nghttp2-corosio/logging.hpp"

#include <boost/capy/buffers.hpp>
#include <boost/capy/detail/buffer_array.hpp>
#include <boost/capy/ex/any_executor.hpp>
#include <boost/capy/io/any_read_stream.hpp>
#include <boost/capy/io/any_write_stream.hpp>
#include <boost/capy/io_task.hpp>
#include <boost/capy/read.hpp>
#include <boost/capy/write.hpp>

#include <functional>
#include <memory>
#include <span>
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
   /// client session. Any concrete type satisfying capy's ReadStream concept can be type-erased
   /// into one of these -- see https://develop.capy.cpp.al/capy/6.streams/6.intro.html.
   using Reader = boost::capy::any_read_stream;

   /// A writable body: the outgoing response on a server session, or the outgoing request on a
   /// client session. Any concrete type satisfying capy's WriteStream concept can be type-erased
   /// into one of these -- see https://develop.capy.cpp.al/capy/6.streams/6.intro.html. Note this
   /// covers only write_some()/write(); write_eof() (signaling HTTP/2 END_STREAM) isn't part of
   /// capy's WriteStream, so Response/ClientRequest carry it separately as a WriteEofFn callback.
   using Writer = boost::capy::any_write_stream;

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
      boost::capy::io_task<std::size_t> read_some(MB buffers)
      {
         co_return co_await reader_.read_some(buffers);
      }

      template <boost::capy::MutableBufferSequence MB>
      boost::capy::io_task<std::size_t> read(MB buffers)
      {
         return boost::capy::read(reader_, buffers);
      }

   private:
      std::string path_;
      Reader reader_;
   };

   /// The incoming response on a client session: the `:status` pseudo-header -- already known by
   /// the time a caller has one of these, see ClientRequest::get_response() -- plus the response
   /// body. ClientResponse forwards read_some()/read() to the body, so it can be used directly
   /// wherever a Reader is expected.
   class ClientResponse
   {
   public:
      ClientResponse(unsigned int status, Reader reader)
         : status_(status), reader_(std::move(reader))
      {
      }

      /// The response's `:status` pseudo-header.
      unsigned int status() const noexcept { return status_; }

      template <boost::capy::MutableBufferSequence MB>
      boost::capy::io_task<std::size_t> read_some(MB buffers)
      {
         co_return co_await reader_.read_some(buffers);
      }

   private:
      unsigned int status_;
      Reader reader_;
   };

   /// The outgoing request on a client session: a writable body plus get_response(), which
   /// resolves once the response's `:status` pseudo-header has arrived. ClientRequest forwards
   /// write_some()/write()/write_eof() to the body, so it can be used directly wherever a Writer
   /// is expected.
   ///
   /// Mirrors Response's write+submit() pairing on the server side, in the other direction:
   /// get_response() is the client's one-shot "wait for the peer" step, the way submit() is the
   /// server's one-shot "commit to headers" step. Unlike submit(), get_response() doesn't gate
   /// writing -- the request body can (and, for bodies larger than the HTTP/2 flow-control window,
   /// must) be written concurrently with awaiting it, e.g. via when_all().
   class ClientRequest
   {
   public:
      /// Waits for the response's `:status` pseudo-header to arrive and returns the resulting
      /// ClientResponse; constructed by Session::Impl, which is the only thing that knows how to
      /// ask the underlying Stream.
      using GetResponseFn = std::function<boost::capy::io_task<ClientResponse>()>;

      /// Writes (or, with an empty buffer sequence, just signals) end-of-body and marks the
      /// HTTP/2 stream half-closed. capy's WriteStream concept has no such notion -- there's no
      /// free algorithm to fall back on the way read()/write() do -- so, like SubmitFn/
      /// GetResponseFn, Session::Impl supplies this closed over the concrete Stream, which is the
      /// only thing that knows how to mark a data-provider callback's last chunk.
      using WriteEofFn =
         std::function<boost::capy::io_task<std::size_t>(std::span<boost::capy::const_buffer const>)>;

      ClientRequest(Writer writer, WriteEofFn write_eof, GetResponseFn get_response)
         : writer_(std::move(writer))
         , write_eof_(std::move(write_eof))
         , get_response_(std::move(get_response))
      {
      }

      template <boost::capy::ConstBufferSequence CB>
      boost::capy::io_task<std::size_t> write_some(CB buffers)
      {
         co_return co_await writer_.write_some(buffers);
      }

      template <boost::capy::ConstBufferSequence CB>
      boost::capy::io_task<std::size_t> write(CB buffers)
      {
         co_return co_await boost::capy::write(writer_, buffers);
      }

      template <boost::capy::ConstBufferSequence CB>
      boost::capy::io_task<std::size_t> write_eof(CB buffers)
      {
         // Must be a coroutine, not a plain forwarding function: ba's lifetime needs to span the
         // co_await below, since capy's task<> is lazy -- write_eof_'s body doesn't run (and
         // doesn't copy out of ba) until driven by this co_await.
         boost::capy::detail::const_buffer_array<boost::capy::detail::max_iovec_> ba(buffers);
         co_return co_await write_eof_(ba.to_span());
      }

      boost::capy::io_task<> write_eof()
      {
         auto [ec, n] = co_await write_eof_({});
         (void)n;
         co_return boost::capy::io_result<>{ec};
      }

      /// Waits for the response to arrive. Safe to call more than once -- returns immediately
      /// once the response has already arrived. Safe to await concurrently with writing the
      /// request body (e.g. via when_all()) -- necessary whenever the body is larger than the
      /// HTTP/2 flow-control window, since some handlers won't submit a response until they've
      /// read at least some of the request (see Response's doc comment on submit() timing).
      boost::capy::io_task<ClientResponse> get_response() { return get_response_(); }

   private:
      Writer writer_;
      WriteEofFn write_eof_;
      GetResponseFn get_response_;
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

      /// Writes (or, with an empty buffer sequence, just signals) end-of-body and marks the
      /// HTTP/2 stream half-closed. capy's WriteStream concept has no such notion -- there's no
      /// free algorithm to fall back on the way read()/write() do -- so, like SubmitFn,
      /// Session::Impl supplies this closed over the concrete Stream, which is the only thing
      /// that knows how to mark a data-provider callback's last chunk.
      using WriteEofFn =
         std::function<boost::capy::io_task<std::size_t>(std::span<boost::capy::const_buffer const>)>;

      Response(Writer writer, SubmitFn submit, WriteEofFn write_eof)
         : writer_(std::move(writer)), submit_(std::move(submit)), write_eof_(std::move(write_eof))
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
         co_return co_await boost::capy::write(writer_, buffers);
      }

      template <boost::capy::ConstBufferSequence CB>
      boost::capy::io_task<std::size_t> write_eof(CB buffers)
      {
         // Must be a coroutine, not a plain forwarding function: ba's lifetime needs to span the
         // co_await below, since capy's task<> is lazy -- write_eof_'s body doesn't run (and
         // doesn't copy out of ba) until driven by this co_await.
         boost::capy::detail::const_buffer_array<boost::capy::detail::max_iovec_> ba(buffers);
         co_return co_await write_eof_(ba.to_span());
      }

      boost::capy::io_task<> write_eof()
      {
         auto [ec, n] = co_await write_eof_({});
         (void)n;
         co_return boost::capy::io_result<>{ec};
      }

   private:
      Writer writer_;
      SubmitFn submit_;
      WriteEofFn write_eof_;
   };

   Session() = default;
   explicit Session(std::shared_ptr<Impl> impl);
   Session(Session&& other) noexcept;
   Session& operator=(Session&& other) noexcept;
   ~Session();

   constexpr operator bool() const noexcept { return static_cast<bool>(impl_); }

   using executor_type = boost::capy::any_executor;
   executor_type get_executor() const noexcept;

   /// Submits a request on a new stream (client sessions only) and returns a ClientRequest: a
   /// writable request body plus get_response(), which resolves once the response's `:status`
   /// pseudo-header has arrived. Pseudo-headers beyond :method/:path are currently fixed (POST,
   /// http, an authority derived from the peer endpoint) -- a real headers API will come later.
   boost::capy::io_task<ClientRequest> submit_request(std::string_view path);

private:
   std::shared_ptr<Impl> impl_;
};

// =================================================================================================

} // namespace nghttp2_corosio
