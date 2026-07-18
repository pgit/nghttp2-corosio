#include "utils.hpp"

#include <nghttp2-corosio/server.hpp>

#include <boost/corosio/io_context.hpp>

#include <nghttp2/nghttp2.h>

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace
{

// =================================================================================================
//
// A minimal, synchronous HTTP/2 client used to poke at the server byte-by-byte, without pulling in
// a second coroutine framework (or an external tool like curl -- see README.md for how to do that
// manually) into the test binary.
//
// No thread runs the server's io_context in the background -- this test binary spawns no threads
// at all. Instead, read_exact() below drives `ctx_` (the server's own io_context) directly,
// interleaved with non-blocking recv() attempts on the raw socket, so the two sides take turns
// making progress on a single thread: a plain blocking recv() would deadlock here, since nothing
// would be left to run the server while we sit blocked waiting on it. connect()/send() don't need
// this treatment -- a loopback TCP connect() to a listening socket completes at the kernel level
// regardless of whether the server has called accept() yet, and the small payloads these tests
// send fit comfortably in the kernel's socket send buffer.
//
// =================================================================================================

class RawClient
{
public:
   RawClient(boost::corosio::io_context& ctx, std::uint16_t port) : ctx_(ctx)
   {
      fd_ = ::socket(AF_INET, SOCK_STREAM, 0);

      sockaddr_in addr{};
      addr.sin_family = AF_INET;
      addr.sin_port = htons(port);
      ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

      connected_ = ::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
   }

   ~RawClient()
   {
      if (fd_ >= 0)
         ::close(fd_);
   }

   RawClient(const RawClient&) = delete;
   RawClient& operator=(const RawClient&) = delete;

   bool connected() const noexcept { return connected_; }

   bool send_all(std::string_view data)
   {
      std::size_t sent = 0;
      while (sent < data.size())
      {
         auto n = ::send(fd_, data.data() + sent, data.size() - sent, 0);
         if (n <= 0)
            return false;
         sent += static_cast<std::size_t>(n);
      }
      return true;
   }

   /// Reads exactly `n` bytes, or fewer than `n` on EOF/error/timeout (check the returned size).
   std::vector<std::uint8_t> read_exact(std::size_t n)
   {
      std::vector<std::uint8_t> buffer(n);
      std::size_t got = 0;
      auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
      while (got < n && std::chrono::steady_clock::now() < deadline)
      {
         auto r = ::recv(fd_, buffer.data() + got, n - got, MSG_DONTWAIT);
         if (r > 0)
         {
            got += static_cast<std::size_t>(r);
            continue;
         }
         if (r == 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
            break; // peer closed, or a real error

         // Nothing available yet -- give the server a chance to make progress, then retry.
         ctx_.run_one_for(std::chrono::milliseconds(20));
      }
      buffer.resize(got);
      return buffer;
   }

private:
   boost::corosio::io_context& ctx_;
   int fd_ = -1;
   bool connected_ = false;
};

// -------------------------------------------------------------------------------------------------

struct FrameHeader
{
   std::uint32_t length;
   std::uint8_t type;
   std::uint8_t flags;
   std::int32_t stream_id;
};

/// Reads and parses one HTTP/2 frame header (RFC 9113 section 4.1). Does not consume the payload.
std::optional<FrameHeader> read_frame_header(RawClient& client)
{
   auto bytes = client.read_exact(9);
   if (bytes.size() != 9)
      return std::nullopt;

   return FrameHeader{.length = (static_cast<std::uint32_t>(bytes[0]) << 16) |
                                (static_cast<std::uint32_t>(bytes[1]) << 8) | bytes[2],
                      .type = bytes[3],
                      .flags = bytes[4],
                      .stream_id =
                         static_cast<std::int32_t>(((bytes[5] & 0x7Fu) << 24) | (bytes[6] << 16) |
                                                   (bytes[7] << 8) | bytes[8])};
}

constexpr std::string_view kClientPreface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";

// An empty SETTINGS frame: 9-byte header (length=0, type=SETTINGS, flags=0, stream=0), no payload.
constexpr std::array<std::uint8_t, 9> kEmptyClientSettings{0, 0, 0, NGHTTP2_SETTINGS, 0, 0,
                                                           0, 0, 0};

// =================================================================================================

class ServerTest : public testing::Test
{
protected:
   void SetUp() override
   {
      nghttp2_corosio::Config config;
      config.port = 0; // ask the OS for an unused port

      // See the comment on nghttp2_corosio_test::leak(): destroying a Server whose session
      // hasn't fully wound down reliably crashes, so server_ is deliberately leaked rather than
      // torn down -- safe in this short-lived test binary, and every test gets a fresh instance
      // on its own ephemeral port. No thread runs server_'s io_context in the background either:
      // see the comment on RawClient for how each test drives it directly, interleaved with raw
      // socket I/O.
      server_ = nghttp2_corosio_test::leak(new nghttp2_corosio::Server(config));
   }

   std::uint16_t port() const { return server_->local_endpoint().port(); }
   boost::corosio::io_context& context() const { return server_->get_executor().context(); }

   nghttp2_corosio::Server* server_ = nullptr;
};

// =================================================================================================

TEST_F(ServerTest, AcceptsConnection)
{
   RawClient client(context(), port());
   EXPECT_TRUE(client.connected());
}

TEST_F(ServerTest, SendsInitialSettingsFrame)
{
   RawClient client(context(), port());
   ASSERT_TRUE(client.connected());

   // The server submits its SETTINGS frame as soon as the session is set up, before it has read
   // anything from the peer -- so this is observable without sending the client preface first.
   auto header = read_frame_header(client);
   ASSERT_TRUE(header.has_value());
   EXPECT_EQ(header->type, NGHTTP2_SETTINGS);
   EXPECT_EQ(header->flags, NGHTTP2_FLAG_NONE);
   EXPECT_EQ(header->stream_id, 0);

   auto payload = client.read_exact(header->length);
   EXPECT_EQ(payload.size(), header->length);
}

TEST_F(ServerTest, AcknowledgesClientSettings)
{
   RawClient client(context(), port());
   ASSERT_TRUE(client.connected());

   // Drain the server's initial SETTINGS frame.
   auto initial = read_frame_header(client);
   ASSERT_TRUE(initial.has_value());
   client.read_exact(initial->length);

   ASSERT_TRUE(client.send_all(kClientPreface));
   ASSERT_TRUE(client.send_all(std::string_view(
      reinterpret_cast<const char*>(kEmptyClientSettings.data()), kEmptyClientSettings.size())));

   // nghttp2 acks incoming SETTINGS automatically; look for it among whatever else the server
   // sends back (order relative to other frames isn't guaranteed).
   bool found_ack = false;
   for (int i = 0; i < 10 && !found_ack; ++i)
   {
      auto header = read_frame_header(client);
      ASSERT_TRUE(header.has_value());
      client.read_exact(header->length);
      if (header->type == NGHTTP2_SETTINGS && (header->flags & NGHTTP2_FLAG_ACK))
         found_ack = true;
   }
   EXPECT_TRUE(found_ack);
}

TEST_F(ServerTest, AcceptsMultipleSequentialConnections)
{
   for (int i = 0; i < 3; ++i)
   {
      RawClient client(context(), port());
      ASSERT_TRUE(client.connected()) << "connection " << i;
      EXPECT_TRUE(read_frame_header(client).has_value()) << "connection " << i;
   }
}

TEST_F(ServerTest, SurvivesAbruptDisconnect)
{
   {
      RawClient client(context(), port());
      ASSERT_TRUE(client.connected());
      // Close the socket immediately, without reading or writing anything.
   }

   // The accept loop and session cleanup must still be healthy afterwards.
   RawClient client(context(), port());
   ASSERT_TRUE(client.connected());
   EXPECT_TRUE(read_frame_header(client).has_value());
}

} // namespace
