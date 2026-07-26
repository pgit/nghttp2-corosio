#include "nghttp2-corosio/client.hpp"
#include "client_impl.hpp"
#include "nghttp2-corosio/formatter.hpp"
#include "nghttp2-corosio/logging.hpp"
#include "session_impl.hpp"
#include "task_group.hpp"

#include <boost/capy/io/any_stream.hpp>
#include <boost/corosio/socket_option.hpp>
#include <boost/corosio/tcp_socket.hpp>

namespace capy = boost::capy;
namespace corosio = boost::corosio;

namespace nghttp2_corosio
{

// =================================================================================================

Client::Client(capy::any_executor executor)
   : impl_(std::make_shared<Impl>(std::move(executor)))
{
   logi("Client: ctor");
}

Client::Client(Client&&) noexcept = default;
Client& Client::operator=(Client&&) noexcept = default;
Client::~Client() { logi("Client: dtor"); }

Client::executor_type Client::get_executor() const noexcept { return impl_->get_executor(); }

capy::io_task<Session> Client::connect(corosio::endpoint ep)
{
   return impl_->connect(ep);
}

// =================================================================================================

capy::io_task<Session> Client::Impl::connect(corosio::endpoint ep)
{
   logi("Client: connecting to {}...", ep);
   corosio::tcp_socket socket(executor_);
   if (auto [ec] = co_await socket.connect(ep); ec)
   {
      logw("Client: connecting to {}... failed: {}", ep, ec.message());
      co_return {ec, Session{}};
   }
   logi("Client: connected to {}", ep);

   // See Server::Impl::accept_loop() for why HTTP/2 needs this on both ends of the connection.
   socket.set_option(corosio::socket_option::no_delay(true));

   capy::any_stream stream(std::move(socket));
   auto session = std::make_shared<Session::Impl>(executor_, std::move(stream),
                                                  Session::Impl::Role::client, RequestHandler{});

   // Tracked via the executor's context, not via this Client: Client borrows an external executor
   // (see client.hpp) rather than owning it, so it has no destructor of its own to drain from.
   // Whichever Server (or other owner) does own that context's io_context will cancel and join
   // this session as part of its own teardown -- see Server::Impl::~Impl() and detail::TaskGroup.
   auto& group = executor_.context().use_service<detail::TaskGroup>();
   group.spawn(executor_, session->run());

   co_return {std::error_code{}, Session(std::move(session))};
}

// =================================================================================================

} // namespace nghttp2_corosio
