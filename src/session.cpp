#include "nghttp2-corosio/session.hpp"
#include "session_impl.hpp"

namespace nghttp2_corosio
{

// =================================================================================================

Session::Session(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}
Session::Session(Session&&) noexcept = default;
Session& Session::operator=(Session&&) noexcept = default;
Session::~Session() = default;

Session::executor_type Session::get_executor() const noexcept { return impl_->get_executor(); }

// =================================================================================================

} // namespace nghttp2_corosio
