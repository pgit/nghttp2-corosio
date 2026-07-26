//
// Ported from anyhttp's External fixture
// (https://github.com/pgit/anyhttp/blob/master/test/test_server.cpp), adapted to
// nghttp2-corosio's Server/Session API. Only the tests that make sense for this project's current
// feature set are ported: no HTTP/1.1 (curl_multiple_https/curl_https/nc_crazy_chunked need
// http11/TLS, neither of which exist here yet) and no h2spec (its binary isn't installed in this
// devcontainer, unlike nghttp/curl/h2load).
//
#include "request_handlers.hpp"

#include <nghttp2-corosio/logging.hpp>
#include <nghttp2-corosio/server.hpp>
#include <nghttp2-corosio/session.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/readable_pipe.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <boost/filesystem.hpp>

#include <boost/process/v2/process.hpp>
#include <boost/process/v2/stdio.hpp>

#include <boost/system/system_error.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <format>
#include <future>
#include <optional>
#include <regex>
#include <string>
#include <thread>
#include <vector>

namespace
{

namespace asio = boost::asio;
namespace bp = boost::process::v2;
using namespace boost::asio::experimental::awaitable_operators;

using Args = std::vector<std::string>;

constexpr auto kCurlPath = "/usr/bin/curl";
constexpr auto kNghttpPath = "/usr/local/bin/nghttp";
constexpr auto kH2loadPath = "/usr/local/bin/h2load";

// =================================================================================================
//
// Spawns external HTTP/2 client tools (curl, nghttp, h2load, ...) against a real
// nghttp2_corosio::Server and asserts on what they report back.
//
// anyhttp's own External fixture spawns Boost.Process.V2 straight onto its server's io_context,
// since server and client tooling are both plain ASIO there. This library's Server owns a corosio
// io_context instead, which Process.V2 can't drive directly -- it needs an ASIO executor for its
// pipes and process-exit notification. So this fixture runs a second, plain boost::asio::io_context
// dedicated to process I/O, on its own background thread, alongside the server's corosio context
// on the main thread (the only place in this test binary that spawns a thread at all -- see
// RawClient's doc comment in test_server.cpp for why every other fixture manages to avoid it: a
// raw, non-blocking socket can be polled manually alongside corosio from one thread, but
// Process.V2's pipes have no equivalent non-blocking escape hatch).
//
// The bridge back from that thread is a single cross-thread nghttp2_corosio::Server::stop() call,
// made once every spawned process has finished -- documented as safe to call "on another thread"
// concurrently with a synchronous run() (see Server::stop()'s doc comment in server.hpp), backed by
// corosio::io_context's own thread-safety contract: under its default `locking_mode::safe` (what
// this Server always uses, since nothing here opts into the lockless `unsafe`/`unsafe_io` tiers),
// any thread may call stop() while another drives run().
//
// =================================================================================================

class External : public testing::Test
{
protected:
   void SetUp() override
   {
      nghttp2_corosio::Config config;
      config.port = 0; // ask the OS for an unused port
      config.handler = nghttp2_corosio_test::echo;
      server_.emplace(config);

      io_thread_ = std::thread([this] { io_ctx_.run(); });
   }

   void TearDown() override
   {
      work_guard_.reset();
      io_ctx_.stop();
      if (io_thread_.joinable())
         io_thread_.join();
      server_.reset();
   }

   /// Blocks the calling thread, running the server's own io_context until every process spawned
   /// below has finished and stopped it from the background thread.
   void run() { server_->run(); }

   asio::awaitable<std::string> read_all(asio::readable_pipe pipe)
   {
      std::string result;
      boost::system::error_code ec;
      co_await asio::async_read(pipe, asio::dynamic_buffer(result),
                                asio::redirect_error(asio::use_awaitable, ec));
      if (ec && ec != asio::error::eof)
         throw boost::system::system_error(ec);
      co_return result;
   }

   /// Drains the child's stderr without keeping it (curl/nghttp/h2load are chatty on -v/--verbose)
   /// -- just enough that its OS pipe buffer never fills up and stalls the child, which would
   /// otherwise deadlock alongside read_all() racing to drain stdout concurrently below.
   asio::awaitable<void> drain(asio::readable_pipe pipe)
   {
      std::string buffer;
      boost::system::error_code ec;
      co_await asio::async_read(pipe, asio::dynamic_buffer(buffer),
                                asio::redirect_error(asio::use_awaitable, ec));
      if (!buffer.empty())
         logd("stderr: {}", buffer);
   }

   asio::awaitable<std::string> spawn_process(bp::filesystem::path path,
                                              std::vector<std::string> args)
   {
      std::string joined;
      for (const auto& arg : args)
      {
         if (!joined.empty())
            joined += ' ';
         joined += arg;
      }
      logi("spawn: {} {}", path.generic_string(), joined);

      auto ex = co_await asio::this_coro::executor;
      asio::readable_pipe out(ex), err(ex);
      bp::process child(ex, path, args, bp::process_stdio{.out = out, .err = err});

      auto result = co_await (drain(std::move(err)) && read_all(std::move(out)));

      co_await child.async_wait(asio::use_awaitable);
      if (child.exit_code())
         logw("spawn: {} exited with {}", path.generic_string(), child.exit_code());

      if (--numSpawned_ <= 0)
         server_->stop();

      co_return result;
   }

   std::future<std::string> spawn(bp::filesystem::path path, std::vector<std::string> args)
   {
      ++numSpawned_;
      std::promise<std::string> promise;
      auto future = promise.get_future();
      asio::co_spawn(io_ctx_, spawn_process(std::move(path), std::move(args)),
                     [promise = std::move(promise)](std::exception_ptr ep, std::string str) mutable
      {
         if (ep)
         {
            try
            {
               std::rethrow_exception(ep);
            }
            catch (const std::exception& e)
            {
               str = e.what();
            }
            loge("spawn: {}", str);
         }
         promise.set_value(std::move(str));
      });
      return future;
   }

   asio::io_context io_ctx_;
   asio::executor_work_guard<asio::io_context::executor_type> work_guard_{io_ctx_.get_executor()};
   std::thread io_thread_;
   std::atomic<int> numSpawned_ = 0;

   std::optional<nghttp2_corosio::Server> server_;
   bp::filesystem::path testFile_{"CMakeLists.txt"};
   std::size_t testFileSize_ = bp::filesystem::file_size(testFile_);
};

// -------------------------------------------------------------------------------------------------

TEST_F(External, echo)
{
   auto future = spawn("/usr/bin/echo", {"Hello, World!"});
   run();
   EXPECT_EQ(future.get(), "Hello, World!\n");
}

TEST_F(External, nghttp2)
{
   auto url = std::format("http://127.0.0.1:{}/echo", server_->local_endpoint().port());
   auto future = spawn(kNghttpPath, {"-d", testFile_.string(), url});
   run();

   EXPECT_EQ(future.get().size(), testFileSize_);
}

TEST_F(External, curl)
{
   auto url = std::format("http://127.0.0.1:{}/echo", server_->local_endpoint().port());
   Args args = {"--http2-prior-knowledge",
                "-sS",
                "-v",
                "--data-binary",
                std::format("@{}", testFile_.string()),
                url};

   auto future = spawn(kCurlPath, std::move(args));
   run();

   EXPECT_EQ(future.get().size(), testFileSize_);
}

TEST_F(External, curl_many)
{
   std::vector<std::future<std::string>> futures;
   futures.reserve(10);

   for (std::size_t i = 0; i < futures.capacity(); ++i)
   {
      auto url = std::format("http://127.0.0.1:{}/echo", server_->local_endpoint().port());
      Args args = {"--http2-prior-knowledge",
                   "-sS",
                   "-v",
                   "--data-binary",
                   std::format("@{}", testFile_.string()),
                   url};
      futures.emplace_back(spawn(kCurlPath, std::move(args)));
   }

   run();

   for (auto& future : futures)
      EXPECT_EQ(future.get().size(), testFileSize_);
}

TEST_F(External, curl_multiple)
{
   auto url = std::format("http://127.0.0.1:{}/echo", server_->local_endpoint().port());
   Args args = {"--http2-prior-knowledge",
                "-sS",
                "-v",
                "--data-binary",
                std::format("@{}", testFile_.string()),
                url,
                url};

   auto future = spawn(kCurlPath, std::move(args));
   run();

   EXPECT_EQ(future.get().size(), testFileSize_ * 2);
}

TEST_F(External, h2load)
{
   const std::size_t n = 100; // number of requests, echoing 65535 bytes each
   auto url = std::format("http://127.0.0.1:{}/echo", server_->local_endpoint().port());
   Args args = {"-d", "test/data/64kminus1", "-n", std::to_string(n), "-c", "4", "-m", "3", url};

   auto future = spawn(kH2loadPath, std::move(args));
   run();

   const std::string output = future.get();
   std::smatch match;
   std::regex regex(
      R"((\d+) total, \d+ started, (\d+) done, (\d+) succeeded, (\d+) failed, \d+ errored)");
   ASSERT_TRUE(std::regex_search(output.begin(), output.end(), match, regex)) << output;
   EXPECT_EQ(std::stoul(match[3].str()), n) << match[1];
   EXPECT_EQ(std::stoul(match[4].str()), 0u) << match[1];

   regex = std::regex(R"(\((\d+)\) data)");
   ASSERT_TRUE(std::regex_search(output.begin(), output.end(), match, regex)) << output;
   EXPECT_EQ(std::stoul(match[1].str()), n * 65535) << match[1];
}

} // namespace
