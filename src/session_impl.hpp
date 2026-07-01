#pragma once

#include "nghttp2-corosio/session.hpp"

#include <memory>

namespace nghttp2_corosio
{

// =================================================================================================

class Session::Impl : public std::enable_shared_from_this<Session::Impl>
{
public:
   explicit Impl(boost::capy::any_executor executor) : executor_(std::move(executor)) {}

   boost::capy::any_executor get_executor() const noexcept { return executor_; }

private:
   boost::capy::any_executor executor_;
};

// =================================================================================================

} // namespace nghttp2_corosio
