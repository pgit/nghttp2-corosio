#pragma once

#include <boost/capy/ex/any_executor.hpp>

#include <memory>

namespace nghttp2_corosio
{

// =================================================================================================

class Session
{
public:
   class Impl;

   Session() = default;
   explicit Session(std::shared_ptr<Impl> impl);
   Session(Session&& other) noexcept;
   Session& operator=(Session&& other) noexcept;
   ~Session();

   constexpr operator bool() const noexcept { return static_cast<bool>(impl_); }

   using executor_type = boost::capy::any_executor;
   executor_type get_executor() const noexcept;

private:
   std::shared_ptr<Impl> impl_;
};

// =================================================================================================

} // namespace nghttp2_corosio
