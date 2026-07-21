#pragma once

#include <chrono>
#include <format>
#include <print>
#include <stdexcept>
#include <string_view>

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

/// Parses one of "debug"/"info"/"warn"/"error"/"off" (the names accepted on server_main's command
/// line and by NGHTTP2_COROSIO_LOG_LEVEL, see test/utils.cpp) into a LogLevel. Throws
/// std::invalid_argument on anything else.
inline LogLevel parse_log_level(std::string_view name)
{
   if (name == "debug")
      return LogLevel::debug;
   if (name == "info")
      return LogLevel::info;
   if (name == "warn")
      return LogLevel::warn;
   if (name == "error")
      return LogLevel::error;
   if (name == "off")
      return LogLevel::off;
   throw std::invalid_argument(
      std::format("unknown log level '{}' (want debug/info/warn/error/off)", name));
}

namespace detail
{

constexpr std::string_view log_level_name(LogLevel level)
{
   switch (level)
   {
   case LogLevel::debug:
      return "debug";
   case LogLevel::info:
      return "info";
   case LogLevel::warn:
      return "warn";
   case LogLevel::error:
      return "error";
   default:
      return "off";
   }
}

/// Wall-clock timestamp for the current log line, e.g. "2026-07-21 20:55:31.017".
inline std::string log_timestamp()
{
   using namespace std::chrono;
   return std::format("{:%Y-%m-%d %H:%M:%OS}", time_point_cast<milliseconds>(system_clock::now()));
}

} // namespace detail

// =================================================================================================

} // namespace nghttp2_corosio

// clang-format off
#define NGHTTP2_COROSIO_LOG(level, ...)                                                            \
   do                                                                                              \
   {                                                                                               \
      if ((level) >= ::nghttp2_corosio::log_level())                                               \
      {                                                                                            \
         std::print("[{}] [{}] ", ::nghttp2_corosio::detail::log_timestamp(),                      \
                     ::nghttp2_corosio::detail::log_level_name(level));                            \
         std::println(__VA_ARGS__);                                                                \
      }                                                                                            \
   } while (false)

#define logd(...) NGHTTP2_COROSIO_LOG(::nghttp2_corosio::LogLevel::debug, __VA_ARGS__)
#define logi(...) NGHTTP2_COROSIO_LOG(::nghttp2_corosio::LogLevel::info,  __VA_ARGS__)
#define logw(...) NGHTTP2_COROSIO_LOG(::nghttp2_corosio::LogLevel::warn,  __VA_ARGS__)
#define loge(...) NGHTTP2_COROSIO_LOG(::nghttp2_corosio::LogLevel::error, __VA_ARGS__)

/// Tag-prefixed logging: prepends "[{log_prefix()}] " to the message. Only usable inside a member
/// function where `log_prefix()` (taking no arguments) resolves unqualified -- e.g. Session::Impl,
/// Stream. See Session::Impl::log_prefix(std::int32_t) for the per-stream variant, used explicitly
/// (not through these macros) by the free-standing nghttp2 callbacks in session.cpp.
#define mlogd(x, ...) logd("[{}] " x, log_prefix() __VA_OPT__(,) __VA_ARGS__)
#define mlogi(x, ...) logi("[{}] " x, log_prefix() __VA_OPT__(,) __VA_ARGS__)
#define mlogw(x, ...) logw("[{}] " x, log_prefix() __VA_OPT__(,) __VA_ARGS__)
#define mloge(x, ...) loge("[{}] " x, log_prefix() __VA_OPT__(,) __VA_ARGS__)
// clang-format on
