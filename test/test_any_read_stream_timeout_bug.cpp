// https://github.com/cppalliance/corosio/issues/327

#include <boost/capy.hpp>
#include <boost/capy/test/read_stream.hpp>

#include <boost/corosio.hpp>

#include <gtest/gtest.h>

namespace capy = boost::capy;
namespace corosio = boost::corosio;
using namespace std::chrono_literals;

capy::task<> timeout_read_some()
{
   capy::test::read_stream mock;
   mock.provide("hello world");

   capy::any_read_stream stream(std::move(mock));

   std::array<char, 64> buf;
   std::ignore = co_await corosio::timeout(stream.read_some(capy::make_buffer(buf)), 50ms);
}

TEST(AnyReadStreamTimeoutBug, SegfaultsOnUninitializedAwaitable)
{
   corosio::io_context ioc;
   capy::run_async(ioc.get_executor())(timeout_read_some());
   ioc.run();
}
