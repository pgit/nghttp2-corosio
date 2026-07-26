//
// Ported from a subset of anyhttp's ClientAsync test suite
// (https://github.com/pgit/anyhttp/blob/master/test/test_server.cpp), adapted to
// nghttp2-corosio's capy/corosio-based API. Tests relying on features this library doesn't have
// yet -- arbitrary request/response headers, a URL type with query params, and asio-style
// per-operation cancellation (cancel_after/bind_cancellation_slot) -- are not ported.
//
#include "request_handlers.hpp"
#include "utils.hpp"

#include <nghttp2-corosio/client.hpp>
#include <nghttp2-corosio/logging.hpp>
#include <nghttp2-corosio/server.hpp>

#include <boost/capy/buffers/make_buffer.hpp>
#include <boost/capy/cond.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/when_all.hpp>
#include <boost/capy/when_any.hpp>
#include <boost/corosio/ipv4_address.hpp>
#include <boost/corosio/timeout.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <exception>
#include <functional>
#include <optional>
#include <random>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <tuple>

using namespace std::string_view_literals;
namespace rv = std::ranges::views;
namespace capy = boost::capy;
namespace corosio = boost::corosio;

using namespace std::chrono_literals;

namespace
{

using nghttp2_corosio::Session;

// =================================================================================================
//
// Fixture mirroring anyhttp's ClientAsync: a Server whose request routing is customizable per
// test (via `custom`), plus a helper (run()) that connects a Client and drives a per-test
// coroutine to completion.
//
// Client and server share the *same* io_context (the server's own) -- there's no separate
// context to juggle, and it keeps both sides on one thread the way corosio's socket objects
// expect. No thread is spawned: run() posts the per-test coroutine, then drives that io_context
// itself, synchronously, until the coroutine's completion handler (below) calls server_->stop()
// -- mirroring how anyhttp's ClientAsync fixture runs its io_context in TearDown() only once the
// test coroutine's completion token has reset the server. Since the server's accept loop always
// has an outstanding async accept, the context never runs out of work on its own; stop() is what
// makes run() return.
//
// server_ is a plain value (via optional, for late construction), destroyed in TearDown(): by
// then, run()'s call to nghttp2_corosio_test::run() has already returned control here, so
// nothing is executing inside the io_context that server_ owns. ~Server() cancels and joins the
// accept loop, every session it accepted, and -- since Client::connect() below registers into
// the same per-context TaskGroup (see task_group.hpp) -- the Client's own session too, all
// synchronously, before its io_context is torn down. See Server::Impl::~Impl().
//
// =================================================================================================
class ClientAsync : public testing::Test
{
protected:
   void SetUp() override
   {
      nghttp2_corosio::Config config;
      config.port = 0; // ask the OS for an unused port
      config.handler = [this](Session::Request request, Session::Response response)
      { return dispatch(std::move(request), std::move(response)); };

      server_.emplace(config);
      port_ = server_->local_endpoint().port();
   }

   void TearDown() override { server_.reset(); }

   /// Routes an incoming request: to `custom` if the test set one, otherwise by path, matching
   /// the small set of server-side helpers in request_handlers.hpp.
   capy::task<> dispatch(Session::Request request, Session::Response response)
   {
      if (custom)
      {
         co_await custom(std::move(request), std::move(response));
         co_return;
      }

      if (request.path() == "/echo")
         co_await nghttp2_corosio_test::echo(std::move(request), std::move(response));
      else if (request.path() == "/eat_request")
         co_await nghttp2_corosio_test::eat_request(std::move(request), std::move(response));
      else
         co_await nghttp2_corosio_test::not_found(std::move(response));
   }

   /// Connects a Client on the server's own executor and runs `test_fn` against the resulting
   /// Session.
   capy::task<> run_client(std::function<capy::task<>(Session)> test_fn)
   {
      auto ep = corosio::endpoint(corosio::ipv4_address("127.0.0.1"), port_);
      nghttp2_corosio::Client client(server_->get_executor());
      auto [ec, session] = co_await client.connect(ep);
      if (ec)
         throw std::system_error(ec);
      co_await test_fn(std::move(session));
   }

   /// Lets a background upload run for `yields` scheduling rounds, then stops the server out from
   /// underneath it -- used by ResetServerDuringRequest. A plain member function, not a capturing
   /// lambda invoked as a temporary: see the caveat in test_client.cpp about coroutine lambdas
   /// dangling once their closure (a temporary) is destroyed past the point where they suspend.
   capy::io_task<> stop_server_after(std::size_t yields)
   {
      [[maybe_unused]] auto r = co_await nghttp2_corosio_test::yield(yields);
      server_->stop();
      co_return {};
   }

   /// Runs `test_fn` to completion (via run_client()), driving the server's io_context on this
   /// thread until the coroutine finishes or throws, then rethrows so ASSERT/EXPECT failures
   /// inside `test_fn` are attributed correctly.
   void run(std::function<capy::task<>(Session)> test_fn)
   {
      capy::run_async(server_->get_executor(), [this] { server_->stop(); },
                      [this](std::exception_ptr ep)
      {
         error_ = ep;
         server_->stop();
      })(run_client(std::move(test_fn)));

      nghttp2_corosio_test::run(server_->get_executor().context());

      if (error_)
         std::rethrow_exception(error_);
   }

   std::uint16_t port_ = 0;
   std::optional<nghttp2_corosio::Server> server_;
   std::exception_ptr error_;
   std::function<capy::task<>(Session::Request, Session::Response)> custom;
};

// =================================================================================================

TEST_F(ClientAsync, PostData_ReceivesEcho)
{
   run([](Session session) -> capy::task<>
   {
      auto [ec, request] = co_await session.submit_request("/echo");
      EXPECT_FALSE(ec);

      constexpr std::size_t bytes = 1024 * 1024;
      static constexpr std::array<std::uint8_t, bytes> data{};
      auto [wec, sent, received] = co_await capy::when_all(
         nghttp2_corosio_test::sendAndForceEOF(request, std::span<const std::uint8_t>(data)),
         nghttp2_corosio_test::count(request));
      EXPECT_FALSE(wec);
      EXPECT_EQ(received, bytes);
   });
}

// -------------------------------------------------------------------------------------------------
//
// content_length(): parsed from an explicit `content-length` header on the reading side
// (Request::content_length(), ClientResponse::content_length()), and settable on the writing side
// (Response::content_length(), submit_request()'s optional parameter) to emit one -- see the doc
// comments in session.hpp. request_handlers.cpp's echo() propagates whatever it read onto the
// response it writes, so round-tripping through /echo below exercises both directions at once, with
// and without a header present.
//
// -------------------------------------------------------------------------------------------------

/// Awaits the response concurrently with uploading through `request`, records its content-length
/// header via `content_length`, then drains and returns the body's byte count. A free function
/// rather than an inline coroutine lambda -- see the caveat on coroutine lambdas invoked as
/// temporaries above stop_server_after().
///
/// Needed because /echo (see request_handlers.cpp's echo()) defers submit() until it has read the
/// first request chunk, so -- like PostRange/Timeout_UnlimitedEchoRoundTrip above --
/// get_response() can't be awaited before the client starts sending without deadlocking; both have
/// to run concurrently, e.g. via when_all() alongside sendAndForceEOF().
capy::io_task<std::size_t> get_response_and_count(Session::ClientRequest& request,
                                                  std::optional<std::size_t>& content_length)
{
   auto [ec, response] = co_await request.get_response();
   if (ec)
      co_return {ec, 0u};
   content_length = response.content_length();
   co_return co_await nghttp2_corosio_test::count(response);
}

TEST_F(ClientAsync, ContentLength_PresentOnRequest_EchoedOnResponse)
{
   std::optional<std::size_t> seen_on_server;
   custom = [&](Session::Request request, Session::Response response) -> capy::task<>
   {
      seen_on_server = request.content_length();
      co_await nghttp2_corosio_test::echo(std::move(request), std::move(response));
   };

   constexpr std::size_t bytes = 1024;
   std::optional<std::size_t> seen_on_client;
   run([&](Session session) -> capy::task<>
   {
      auto [ec, request] = co_await session.submit_request("/echo", bytes);
      EXPECT_FALSE(ec);

      static constexpr std::array<std::uint8_t, bytes> data{};
      auto [wec, sent, received] = co_await capy::when_all(
         nghttp2_corosio_test::sendAndForceEOF(request, std::span<const std::uint8_t>(data)),
         get_response_and_count(request, seen_on_client));
      EXPECT_FALSE(wec);
      EXPECT_EQ(received, bytes);
   });

   EXPECT_EQ(seen_on_client, std::make_optional(bytes));
   ASSERT_TRUE(seen_on_server.has_value());
   EXPECT_EQ(*seen_on_server, bytes);
}

TEST_F(ClientAsync, ContentLength_AbsentOnRequest_AbsentOnResponse)
{
   std::optional<std::size_t> seen_on_server = 0; // any value != nullopt, so the test can tell
   custom = [&](Session::Request request, Session::Response response) -> capy::task<>
   {
      seen_on_server = request.content_length();
      co_await nghttp2_corosio_test::echo(std::move(request), std::move(response));
   };

   std::optional<std::size_t> seen_on_client = 0; // ditto
   run([&](Session session) -> capy::task<>
   {
      // No content_length argument -- submit_request() defaults to not sending the header.
      auto [ec, request] = co_await session.submit_request("/echo");
      EXPECT_FALSE(ec);

      constexpr std::size_t bytes = 1024;
      static constexpr std::array<std::uint8_t, bytes> data{};
      auto [wec, sent, received] = co_await capy::when_all(
         nghttp2_corosio_test::sendAndForceEOF(request, std::span<const std::uint8_t>(data)),
         get_response_and_count(request, seen_on_client));
      EXPECT_FALSE(wec);
      EXPECT_EQ(received, bytes);
   });

   EXPECT_FALSE(seen_on_server.has_value());
   EXPECT_FALSE(seen_on_client.has_value());
}

TEST_F(ClientAsync, ContentLength_SetOnResponse_SeenByClient)
{
   static constexpr auto body = "Hello, World!"sv;
   custom = [](Session::Request request, Session::Response response) -> capy::task<>
   {
      std::ignore = request;
      response.content_length(body.size());
      [[maybe_unused]] auto s = co_await response.submit();
      [[maybe_unused]] auto r = co_await response.write_eof(capy::make_buffer(body));
   };

   run([](Session session) -> capy::task<>
   {
      auto [ec, request] = co_await session.submit_request("/");
      EXPECT_FALSE(ec);
      auto [wec] = co_await request.write_eof();
      EXPECT_FALSE(wec);

      auto [gec, response] = co_await request.get_response();
      EXPECT_FALSE(gec);
      EXPECT_EQ(response.content_length(), std::make_optional(body.size()));

      auto [rec, received] = co_await nghttp2_corosio_test::count(response);
      EXPECT_EQ(received, body.size());
   });
}

TEST_F(ClientAsync, PostToUnknownPath_Returns404)
{
   run([](Session session) -> capy::task<>
   {
      auto [ec, request] = co_await session.submit_request("/unknown");
      EXPECT_FALSE(ec);

      auto [wec] = co_await request.write_eof();
      EXPECT_FALSE(wec);

      auto [sec, response] = co_await request.get_response();
      EXPECT_FALSE(sec);
      EXPECT_EQ(response.status(), 404u);

      auto [cec, received] = co_await nghttp2_corosio_test::count(response);
      EXPECT_EQ(received, 0u);
   });
}

TEST_F(ClientAsync, HelloWorld)
{
   static constexpr auto hello = "Hello, World!"sv;
   custom = [](Session::Request request, Session::Response response) -> capy::task<>
   {
      std::ignore = request;
      [[maybe_unused]] auto r0 = co_await response.submit();
      [[maybe_unused]] auto r1 = co_await response.write_eof(capy::make_buffer(hello));
      // [[maybe_unused]] auto r2 = co_await response.write_eof();
   };

   run([](Session session) -> capy::task<>
   {
      auto [ec, request] = co_await session.submit_request("/");
      EXPECT_FALSE(ec);
      auto [wec] = co_await request.write_eof();
      auto [rec, body] = co_await nghttp2_corosio_test::read(request);
      EXPECT_EQ(body, hello);
   });
}

TEST_F(ClientAsync, Custom)
{
   custom = [](Session::Request request, Session::Response response) -> capy::task<>
   {
      [[maybe_unused]] auto submitted = co_await response.submit();
      std::array<std::uint8_t, 1024> buffer;
      for (;;)
      {
         auto [rec, n] = co_await request.read_some(capy::make_buffer(buffer));
         auto [wec, wn] = co_await response.write(capy::make_buffer(buffer, n));
         if (rec || wec)
            break;
      }
      [[maybe_unused]] auto r = co_await response.write_eof();
   };

   run([](Session session) -> capy::task<>
   {
      auto [ec, request] = co_await session.submit_request("/");
      EXPECT_FALSE(ec);

      constexpr std::size_t bytes = 1024;
      auto [wec, sent, received] = co_await capy::when_all(
         nghttp2_corosio_test::send(request, bytes), nghttp2_corosio_test::count(request));
      EXPECT_EQ(received, bytes);
   });
}

TEST_F(ClientAsync, IgnoreRequest)
{
   custom = [](Session::Request request, Session::Response response) -> capy::task<>
   {
      std::ignore = request;
      [[maybe_unused]] auto s = co_await response.submit();
      [[maybe_unused]] auto r = co_await response.write_eof();
   };

   run([](Session session) -> capy::task<>
   {
      auto [ec, request] = co_await session.submit_request("/");
      EXPECT_FALSE(ec);

      auto [wec, sent, received] = co_await capy::when_all(nghttp2_corosio_test::send(request, 0),
                                                           nghttp2_corosio_test::count(request));
      EXPECT_EQ(received, 0u);
   });
}

TEST_F(ClientAsync, EatRequest)
{
   run([](Session session) -> capy::task<>
   {
      auto [ec, request] = co_await session.submit_request("/eat_request");
      EXPECT_FALSE(ec);

      constexpr std::size_t bytes = 1024;
      auto [wec, sent, received] = co_await capy::when_all(
         nghttp2_corosio_test::send(request, bytes), nghttp2_corosio_test::count(request));
      EXPECT_EQ(received, 0u);
   });
}

// -------------------------------------------------------------------------------------------------
//
// Range support: a request body larger than the default HTTP/2 flow-control window (64KiB) must
// be sent concurrently with reading the response, or both sides deadlock (the server's echo
// handler can't make room in its outgoing window without the client reading; the client can't
// finish sending without the server reading). Ported from anyhttp's PostRange/PostRangeImmediate,
// which differ there only in when the response is fetched relative to the send -- a distinction
// that doesn't exist here, since get_response() can simply be awaited concurrently with the send
// (e.g. via when_all(), same as draining the response concurrently below).
//
// -------------------------------------------------------------------------------------------------

TEST_F(ClientAsync, PostRange)
{
   run([](Session session) -> capy::task<>
   {
      auto [ec, request] = co_await session.submit_request("/echo");
      EXPECT_FALSE(ec);

      constexpr std::size_t bytes = 1 * 1024 * 1024;
      auto [wec, sent, received] =
         co_await capy::when_all(nghttp2_corosio_test::sendAndForceEOF(
                                    request, rv::iota(std::uint8_t{0}) | rv::take(bytes)),
                                 nghttp2_corosio_test::count(request));
      EXPECT_FALSE(wec);
      EXPECT_EQ(received, bytes);
   });
}

// -------------------------------------------------------------------------------------------------

TEST_F(ClientAsync, MultipleRequests_ResponsesInOrder)
{
   run([](Session session) -> capy::task<>
   {
      auto [ec1, request1] = co_await session.submit_request("/echo");
      EXPECT_FALSE(ec1);
      auto [wec1, wn1] = co_await request1.write_eof(capy::make_buffer("Hello, Server #1!"sv));
      EXPECT_FALSE(wec1);

      auto [ec2, request2] = co_await session.submit_request("/echo");
      EXPECT_FALSE(ec2);
      auto [wec2, wn2] = co_await request2.write_eof(capy::make_buffer("Hello, Server #2! XYZ"sv));
      EXPECT_FALSE(wec2);

      auto [rec1, body1] = co_await nghttp2_corosio_test::read(request1);
      EXPECT_EQ(body1.size(), 17u);

      auto [rec2, body2] = co_await nghttp2_corosio_test::read(request2);
      EXPECT_EQ(body2.size(), 21u);
   });
}

// -------------------------------------------------------------------------------------------------

TEST_F(ClientAsync, ClientDropsRequest)
{
   run([](Session session) -> capy::task<>
   {
      auto [ec, request] = co_await session.submit_request("/echo");
      EXPECT_FALSE(ec);
      // `request` goes out of scope right away, without being written to or read from -- the
      // server must not choke on an abandoned stream.
   });
}

// -------------------------------------------------------------------------------------------------

// Ported from anyhttp's ResetServerDuringRequest: races an endless upload against stop()ping the
// server out from underneath it, mid-request. This used to be exactly the scenario CLAUDE.md's
// former "Known issue" warned about -- destroying a Server while a session hadn't fully wound
// down could hang or corrupt memory -- so this test stayed disabled as a regression case to
// enable once that was fixed. It now passes reliably (verified with 100x repeated runs and a
// full-suite valgrind sweep): ~Server()'s structured shutdown (detail::TaskGroup, see
// task_group.hpp) cancels and joins the session synchronously before its io_context is torn
// down, so it no longer matters whether the session had wound down on its own first.
TEST_F(ClientAsync, ResetServerDuringRequest)
{
   run([this](Session session) -> capy::task<>
   {
      auto [ec, request] = co_await session.submit_request("/echo");
      EXPECT_FALSE(ec);

      // Race an endless upload against resetting the server underneath it; when_any() cancels
      // whichever child hasn't finished once the other one has.
      [[maybe_unused]] auto result = co_await capy::when_any(
         nghttp2_corosio_test::send(request, rv::iota(std::uint8_t{0})), stop_server_after(10));

      auto [rec, received] = co_await nghttp2_corosio_test::count(request);
      logd("ResetServerDuringRequest: received {} bytes ({})", received, rec.message());
   });
}

// -------------------------------------------------------------------------------------------------

TEST_F(ClientAsync, ServerYieldFirst)
{
   custom = [](Session::Request request, Session::Response response) -> capy::task<>
   {
      std::ignore = request;
      [[maybe_unused]] auto y1 = co_await nghttp2_corosio_test::yield(10);
      [[maybe_unused]] auto r1 = co_await response.submit();
      [[maybe_unused]] auto y2 = co_await nghttp2_corosio_test::yield(10);
      [[maybe_unused]] auto r2 = co_await response.write_eof();
   };

   run([](Session session) -> capy::task<>
   {
      auto [ec, request] = co_await session.submit_request("/");
      EXPECT_FALSE(ec);
      auto [wec] = co_await request.write_eof();
      auto [rec, received] = co_await nghttp2_corosio_test::count(request);
      EXPECT_EQ(received, 0u);
   });
}

// -------------------------------------------------------------------------------------------------
//
// Fuzzes the interleaving of yields between client and server around request/response submission
// -- a regression test for scheduler edge cases like the one described in CLAUDE.md's "Known
// issue" section. Uses a fixed seed for reproducibility; a smaller iteration count than
// anyhttp's original (which runs 100) to keep the test fast.
//
// -------------------------------------------------------------------------------------------------

TEST_F(ClientAsync, YieldFuzz)
{
   static std::mt19937 gen(42);

   custom = [](Session::Request request, Session::Response response) -> capy::task<>
   {
      std::ignore = request;
      static constexpr auto msg = "Hello, Client!"sv;
      std::uniform_int_distribution<> dist(0, 10);
      [[maybe_unused]] auto y1 = co_await nghttp2_corosio_test::yield(dist(gen));
      [[maybe_unused]] auto s = co_await response.submit();
      [[maybe_unused]] auto r1 = co_await response.write(capy::make_buffer(msg));
      [[maybe_unused]] auto y2 = co_await nghttp2_corosio_test::yield(dist(gen));
      [[maybe_unused]] auto r2 = co_await response.write_eof();
      [[maybe_unused]] auto y3 = co_await nghttp2_corosio_test::yield(dist(gen));
   };

   run([](Session session) -> capy::task<>
   {
      std::uniform_int_distribution<> dist(0, 10);
      for (std::size_t i = 0; i < 20; ++i)
      {
         [[maybe_unused]] auto y1 = co_await nghttp2_corosio_test::yield(dist(gen));
         auto [ec, request] = co_await session.submit_request("/");
         EXPECT_FALSE(ec);
         [[maybe_unused]] auto y2 = co_await nghttp2_corosio_test::yield(dist(gen));
         auto [wec] = co_await request.write_eof();
         [[maybe_unused]] auto y3 = co_await nghttp2_corosio_test::yield(dist(gen));
         [[maybe_unused]] auto received = co_await nghttp2_corosio_test::count(request);
      }
   });
}

// =================================================================================================
//
// EOF handling, partial success, and timeouts -- ported from the corosio guide:
// https://develop.corosio.cpp.al/corosio/4.guide/4m.error-handling.html#_eof_handling
//
// The guide specifies that end-of-stream is signalled by capy::cond::eof, that the composed
// read() operation reports the bytes transferred so far alongside the error (partial success),
// and that corosio::timeout() produces capy::cond::timeout (not capy::cond::canceled) when the
// deadline fires before data arrives.
//
// =================================================================================================

// -------------------------------------------------------------------------------------------------
// EOF handling: verify that the last read_some() on a finished stream returns capy::cond::eof.
// -------------------------------------------------------------------------------------------------

TEST_F(ClientAsync, EofHandling_RequestReadSome)
{
   std::error_code eof_ec;
   custom = [&eof_ec](Session::Request request, Session::Response response) -> capy::task<>
   {
      std::array<std::uint8_t, 1024> buf;
      for (;;)
      {
         auto [ec, n] = co_await request.read_some(capy::make_buffer(buf));
         if (ec)
         {
            eof_ec = ec;
            break;
         }
      }
      [[maybe_unused]] auto s = co_await response.submit();
      [[maybe_unused]] auto r = co_await response.write_eof();
   };

   run([](Session session) -> capy::task<>
   {
      auto [ec, request] = co_await session.submit_request("/");
      EXPECT_FALSE(ec);
      auto [wec, wn] = co_await request.write_eof(capy::make_buffer("hello"sv));
      EXPECT_FALSE(wec);
      auto [rec, body] = co_await nghttp2_corosio_test::read(request);
      EXPECT_FALSE(rec);
   });

   EXPECT_EQ(eof_ec, capy::cond::eof);
}

TEST_F(ClientAsync, EofHandling_ResponseReadSome)
{
   custom = [](Session::Request request, Session::Response response) -> capy::task<>
   {
      std::ignore = request;
      [[maybe_unused]] auto s = co_await response.submit();
      [[maybe_unused]] auto r = co_await response.write_eof(capy::make_buffer("world"sv));
   };

   run([](Session session) -> capy::task<>
   {
      auto [ec, request] = co_await session.submit_request("/");
      EXPECT_FALSE(ec);
      [[maybe_unused]] auto wr = co_await request.write_eof();

      auto [gec, response] = co_await request.get_response();
      EXPECT_FALSE(gec);

      std::array<char, 1024> buf;
      std::error_code eof_ec;
      for (;;)
      {
         auto [rec, n] = co_await response.read_some(capy::make_buffer(buf));
         if (rec)
         {
            eof_ec = rec;
            break;
         }
      }
      EXPECT_EQ(eof_ec, capy::cond::eof);
   });
}

// -------------------------------------------------------------------------------------------------
// Partial success: read() reports the bytes transferred alongside capy::cond::eof when the stream
// ends before the buffer is full (not just error + 0).
// -------------------------------------------------------------------------------------------------

TEST_F(ClientAsync, PartialSuccess_RequestRead)
{
   static constexpr auto payload = "1234567"sv; // 7 bytes, smaller than the 1024-byte buffer
   std::error_code partial_ec;
   std::size_t partial_n = 0;

   custom = [&](Session::Request request, Session::Response response) -> capy::task<>
   {
      std::array<std::uint8_t, 1024> buf;
      auto [ec, n] = co_await request.read(capy::make_buffer(buf));
      partial_ec = ec;
      partial_n = n;
      [[maybe_unused]] auto s = co_await response.submit();
      [[maybe_unused]] auto r = co_await response.write_eof();
   };

   run([](Session session) -> capy::task<>
   {
      auto [ec, request] = co_await session.submit_request("/");
      EXPECT_FALSE(ec);
      auto [wec, wn] = co_await request.write_eof(capy::make_buffer(payload));
      EXPECT_FALSE(wec);
      [[maybe_unused]] auto r = co_await nghttp2_corosio_test::count(request);
   });

   EXPECT_EQ(partial_ec, capy::cond::eof);
   EXPECT_EQ(partial_n, payload.size());
}

TEST_F(ClientAsync, PartialSuccess_ResponseRead)
{
   static constexpr auto payload = "1234567"sv; // 7 bytes, smaller than the 1024-byte buffer
   custom = [](Session::Request request, Session::Response response) -> capy::task<>
   {
      std::ignore = request;
      [[maybe_unused]] auto s = co_await response.submit();
      [[maybe_unused]] auto r = co_await response.write_eof(capy::make_buffer(payload));
   };

   run([](Session session) -> capy::task<>
   {
      auto [ec, request] = co_await session.submit_request("/");
      EXPECT_FALSE(ec);
      [[maybe_unused]] auto wr = co_await request.write_eof();

      auto [gec, response] = co_await request.get_response();
      EXPECT_FALSE(gec);

      std::array<char, 1024> buf;
      auto [rec, n] = co_await capy::read(response, capy::make_buffer(buf));
      EXPECT_EQ(rec, capy::cond::eof);
      EXPECT_EQ(n, payload.size());
   });
}

// -------------------------------------------------------------------------------------------------
// Timeouts: wrapping a read_some() in corosio::timeout() produces capy::cond::timeout (not
// capy::cond::canceled) when the deadline elapses before any data arrives.
// -------------------------------------------------------------------------------------------------

TEST_F(ClientAsync, Timeout_RequestReadSome)
{
   std::error_code timeout_ec;
   custom = [&timeout_ec](Session::Request request, Session::Response response) -> capy::task<>
   {
      std::array<std::uint8_t, 1024> buf;
      // Client never sends body data, so the read_some suspends until the 50ms deadline fires.
      auto [ec, n] = co_await corosio::timeout(request.read_some(capy::make_buffer(buf)), 50ms);
      timeout_ec = ec;
      // Respond so the client-side read(response) can complete and stop the server.
      [[maybe_unused]] auto s = co_await response.submit();
      [[maybe_unused]] auto r = co_await response.write_eof();
   };

   run([](Session session) -> capy::task<>
   {
      auto [ec, request] = co_await session.submit_request("/");
      EXPECT_FALSE(ec);
      // `request` stays alive (no body sent) while the server's read_some times out.
      // Once the server responds, drain the response so run_client's completion fires.
      auto [rec, body] = co_await nghttp2_corosio_test::read(request);
      EXPECT_FALSE(rec);
   });

   EXPECT_EQ(timeout_ec, capy::cond::timeout);
   EXPECT_NE(timeout_ec, capy::cond::canceled);
}

TEST_F(ClientAsync, Timeout_ResponseReadSome)
{
   custom = [](Session::Request request, Session::Response response) -> capy::task<>
   {
      std::ignore = request;
      [[maybe_unused]] auto s = co_await response.submit();
      // Delay before sending any body -- long enough that the client's 50ms timeout fires first.
      [[maybe_unused]] auto d = co_await nghttp2_corosio_test::sleep(500ms);
      [[maybe_unused]] auto r = co_await response.write_eof();
   };

   run([](Session session) -> capy::task<>
   {
      auto [ec, request] = co_await session.submit_request("/");
      EXPECT_FALSE(ec);
      [[maybe_unused]] auto wr = co_await request.write_eof();

      // The server calls submit() before its 500ms delay, so the status arrives promptly --
      // get_response() resolves well before the read below times out.
      auto [gec, response] = co_await request.get_response();
      EXPECT_FALSE(gec);

      std::array<char, 1024> buf;
      // Server delays 500ms before sending body data; our 50ms deadline fires first.
      auto [rec, n] = co_await corosio::timeout(response.read_some(capy::make_buffer(buf)), 50ms);
      EXPECT_EQ(rec, capy::cond::timeout);
      EXPECT_NE(rec, capy::cond::canceled);
      EXPECT_EQ(n, 0u);
   });
}

// -------------------------------------------------------------------------------------------------
//
// An unlimited upload/echo round trip, bounded only by a deadline -- how many bytes can the
// server's /echo handler turn around before corosio::timeout() cuts it off? send() (client ->
// server) and count_into() (server's echo -> client) have to run concurrently, the same as
// PostRange above: once the body exceeds the HTTP/2 flow-control window (64KiB), the server can't
// make room to echo more without the client reading, and the client can't finish sending without
// the server reading. Both are raced together inside a single when_all(), which is itself what's
// wrapped in the deadline.
//
// -------------------------------------------------------------------------------------------------

TEST_F(ClientAsync, Timeout_UnlimitedEchoRoundTrip)
{
   run([](Session session) -> capy::task<>
   {
      auto [ec, request] = co_await session.submit_request("/echo");
      EXPECT_FALSE(ec);

      // `received` is a side channel, not read back from the timeout's result -- see
      // count_into()'s doc comment for why the return value can't be trusted here.
      std::size_t received = 0;
      static constexpr auto deadline = 300ms;
      auto [tec, s1, s2] = co_await corosio::timeout(
         capy::when_all(nghttp2_corosio_test::send(request, rv::iota(std::uint8_t{0})),
                        nghttp2_corosio_test::count_into(request, received)),
         deadline);
      EXPECT_EQ(tec, capy::cond::timeout);
      EXPECT_NE(tec, capy::cond::canceled);

      logi("Timeout_UnlimitedEchoRoundTrip: echoed {} bytes in {}ms", received, deadline.count());
      EXPECT_GT(received, 0u);
   });
}

// -------------------------------------------------------------------------------------------------
//
// Ported from anyhttp's WHEN_client_cancels_write_THEN_can_resume: measures how much backpressure
// the echo loop can absorb before the client starts draining the response at all. The server's
// /echo handler writes back everything it reads in lockstep (see request_handlers.cpp's echo()),
// so with nothing draining the response, its outgoing window eventually closes, which stalls its
// reads of the request body, which in turn closes the window the client is sending into -- once
// that happens, even write_eof() alone can't complete until the window reopens, which only happens
// once the client starts draining the response.
//
// -------------------------------------------------------------------------------------------------

TEST_F(ClientAsync, ClientCancelsWrite_CanResume)
{
   run([](Session session) -> capy::task<>
   {
      auto [ec, request] = co_await session.submit_request("/echo");
      EXPECT_FALSE(ec);

      // Send as much data as possible within a budget, without draining the response at all --
      // expect backpressure to close the window before the deadline fires. `sent` is the answer
      // to "how much backpressure can the echo loop take before starting to receive", captured as
      // a side channel for the same reason as count_into() above -- the timeout's own result
      // won't survive, and send() doesn't report a byte count to begin with.
      std::size_t sent = 0;
      static constexpr auto budget = 1s;
      auto [tec] = co_await corosio::timeout(
         nghttp2_corosio_test::send_into(request, rv::iota(std::uint8_t{0}), sent), budget);
      EXPECT_EQ(tec, capy::cond::timeout);
      logi("ClientCancelsWrite_CanResume: {} bytes absorbed before the window closed", sent);
      EXPECT_GT(sent, 0u);

      // With the window closed, even ending the upload alone can't complete immediately.
      auto [eof_tec] = co_await corosio::timeout(request.write_eof(), 1ms);
      EXPECT_EQ(eof_tec, capy::cond::timeout);

      // We don't control when the window reopens -- only draining the response does that -- so
      // race the (now unbounded) EOF write against reading the response, concurrently.
      std::size_t received = 0;
      auto [wec, s1, s2] = co_await capy::when_all(
         request.write_eof(), nghttp2_corosio_test::count_into(request, received));
      EXPECT_FALSE(wec);

      logi("ClientCancelsWrite_CanResume: {} bytes received after resuming", received);
      EXPECT_GT(received, 0u);
   });
}

} // namespace
