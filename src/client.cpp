#include "nghttp2-corosio/client.hpp"
#include "client_impl.hpp"
#include "session_impl.hpp"

#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/io/any_stream.hpp>
#include <boost/corosio/tcp_socket.hpp>

namespace nghttp2_corosio
{

// =================================================================================================

Client::Client(boost::capy::any_executor executor)
   : impl_(std::make_shared<Impl>(std::move(executor)))
{
}

Client::Client(Client&&) noexcept = default;
Client& Client::operator=(Client&&) noexcept = default;
Client::~Client() = default;

Client::executor_type Client::get_executor() const noexcept { return impl_->get_executor(); }

boost::capy::io_task<Session> Client::connect(boost::corosio::endpoint ep)
{
   return impl_->connect(ep);
}

// =================================================================================================

boost::capy::io_task<Session> Client::Impl::connect(boost::corosio::endpoint ep)
{
   boost::corosio::tcp_socket socket(executor_);
   if (auto [ec] = co_await socket.connect(ep); ec)
      co_return {ec, Session{}};

   boost::capy::any_stream stream(std::move(socket));
   auto session = std::make_shared<Session::Impl>(executor_, std::move(stream),
                                                   Session::Impl::Role::client, RequestHandler{});
   boost::capy::run_async(executor_)(session->run());

   co_return {std::error_code{}, Session(std::move(session))};
}

// =================================================================================================

} // namespace nghttp2_corosio
