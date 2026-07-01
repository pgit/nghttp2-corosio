#include <nghttp2-corosio/server.hpp>

#include <nghttp2/nghttp2.h>

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>
#include <thread>
#include <vector>

namespace
{

// =================================================================================================
//
// A minimal, synchronous HTTP/2 client used to poke at the server byte-by-byte, without pulling in
// a second coroutine framework (or an external tool like curl -- see README.md for how to do that
// manually) into the test binary.
//
// =================================================================================================

class RawClient
{
public:
   explicit RawClient(std::uint16_t port)
   {
      fd_ = ::socket(AF_INET, SOCK_STREAM, 0);

      // Don't let a stuck test hang the whole suite if the server never responds.
      timeval timeout{.tv_sec = 2, .tv_usec = 0};
      ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

      sockaddr_in addr{};
      addr.sin_family = AF_INET;
      addr.sin_port = htons(port);
      ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

      connected_ =
         ::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
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
      while (got < n)
      {
         auto r = ::recv(fd_, buffer.data() + got, n - got, 0);
         if (r <= 0)
         {
            buffer.resize(got);
            break;
         }
         got += static_cast<std::size_t>(r);
      }
      return buffer;
   }

private:
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

   return FrameHeader{
      .length = (static_cast<std::uint32_t>(bytes[0]) << 16) |
                (static_cast<std::uint32_t>(bytes[1]) << 8) | bytes[2],
      .type = bytes[3],
      .flags = bytes[4],
      .stream_id = static_cast<std::int32_t>(
         ((bytes[5] & 0x7Fu) << 24) | (bytes[6] << 16) | (bytes[7] << 8) | bytes[8])};
}

constexpr std::string_view kClientPreface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";

// An empty SETTINGS frame: 9-byte header (length=0, type=SETTINGS, flags=0, stream=0), no payload.
constexpr std::array<std::uint8_t, 9> kEmptyClientSettings{0, 0, 0, NGHTTP2_SETTINGS, 0, 0, 0, 0, 0};

// =================================================================================================

class ServerTest : public testing::Test
{
protected:
   void SetUp() override
   {
      nghttp2_corosio::Config config;
      config.port = 0; // ask the OS for an unused port

      // server_ and its background thread are deliberately never torn down. Destroying a Server
      // while one of its sessions hasn't fully wound down can deadlock: recv_loop()'s final
      // start_write() (see session.cpp), meant to wake a send_loop() that's parked waiting to
      // flush the closing GOAWAY, is occasionally missed, leaking that session's coroutine and
      // its socket -- which then hangs (or, if forced along, corrupts memory) in ~Server() /
      // ~io_context() while it waits for that leaked I/O object to go away. That looks like a
      // corosio scheduler issue around same-thread wakeups right before the scheduler goes idle;
      // it needs investigation upstream. Until then, leaking here (safe: this is a short-lived
      // test binary, and every test gets a fresh instance on its own ephemeral port) lets these
      // tests exercise the real wire protocol without hitting that race.
      server_ = new nghttp2_corosio::Server(config);
      std::thread([server = server_] { server->run(); }).detach();
   }

   std::uint16_t port() const { return server_->local_endpoint().port(); }

   nghttp2_corosio::Server* server_ = nullptr;
};

// =================================================================================================

TEST_F(ServerTest, AcceptsConnection)
{
   RawClient client(port());
   EXPECT_TRUE(client.connected());
}

TEST_F(ServerTest, SendsInitialSettingsFrame)
{
   RawClient client(port());
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
   RawClient client(port());
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
      RawClient client(port());
      ASSERT_TRUE(client.connected()) << "connection " << i;
      EXPECT_TRUE(read_frame_header(client).has_value()) << "connection " << i;
   }
}

TEST_F(ServerTest, SurvivesAbruptDisconnect)
{
   {
      RawClient client(port());
      ASSERT_TRUE(client.connected());
      // Close the socket immediately, without reading or writing anything.
   }

   // The accept loop and session cleanup must still be healthy afterwards.
   RawClient client(port());
   ASSERT_TRUE(client.connected());
   EXPECT_TRUE(read_frame_header(client).has_value());
}

} // namespace
