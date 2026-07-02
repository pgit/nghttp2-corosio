#pragma once

#include <print>

namespace nghttp2_corosio
{

// =================================================================================================

/// Ordered so a numerically higher level is more severe; a message logs if its level is >= the
/// configured threshold. `off` suppresses everything, including errors.
enum class LogLevel
{
   debug,
   info,
   warn,
   error,
   off,
};

/// The global log level threshold; anything below it is suppressed. Defaults to `info`. Not
/// synchronized -- set it once at startup (e.g. from main()) before spinning up a Server/Client.
inline LogLevel& log_level()
{
   static LogLevel level = LogLevel::info;
   return level;
}

inline void set_log_level(LogLevel level) { log_level() = level; }

// =================================================================================================

} // namespace nghttp2_corosio

// clang-format off
#define NGHTTP2_COROSIO_LOG(level, ...)                                                            \
   do                                                                                              \
   {                                                                                               \
      if ((level) >= ::nghttp2_corosio::log_level())                                               \
         std::println(__VA_ARGS__);                                                                \
   } while (false)

#define logd(...) NGHTTP2_COROSIO_LOG(::nghttp2_corosio::LogLevel::debug, __VA_ARGS__)
#define logi(...) NGHTTP2_COROSIO_LOG(::nghttp2_corosio::LogLevel::info,  __VA_ARGS__)
#define logw(...) NGHTTP2_COROSIO_LOG(::nghttp2_corosio::LogLevel::warn,  __VA_ARGS__)
#define loge(...) NGHTTP2_COROSIO_LOG(::nghttp2_corosio::LogLevel::error, __VA_ARGS__)
// clang-format on
