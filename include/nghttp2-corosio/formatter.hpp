#pragma once

/// std::format support for external types that only provide an iostream operator<< (corosio's
/// address types) or no formatting at all (nghttp2's C types), ported from anyhttp's
/// formatter.hpp: https://github.com/pgit/anyhttp/blob/develop/include/anyhttp/formatter.hpp

#include <boost/corosio/endpoint.hpp>

#include <nghttp2/nghttp2.h>

#include <algorithm>
#include <format>
#include <ranges>
#include <sstream>

// =================================================================================================

/// Defines a std::formatter<X> that forwards to X's operator<<(std::ostream&, const X&), for types
/// that support streaming but not std::format directly.
#define NGHTTP2_COROSIO_ENABLE_FMT_OSTREAM(X)                                                      \
   template <>                                                                                     \
   struct std::formatter<X> : std::formatter<std::string>                                          \
   {                                                                                               \
      template <typename FormatContext>                                                            \
      auto format(const X& value, FormatContext& ctx) const                                        \
      {                                                                                            \
         std::ostringstream oss;                                                                   \
         oss << value;                                                                             \
         return std::formatter<std::string>::format(oss.str(), ctx);                               \
      }                                                                                            \
   }

NGHTTP2_COROSIO_ENABLE_FMT_OSTREAM(boost::corosio::ipv4_address);
NGHTTP2_COROSIO_ENABLE_FMT_OSTREAM(boost::corosio::ipv6_address);

#undef NGHTTP2_COROSIO_ENABLE_FMT_OSTREAM

// -------------------------------------------------------------------------------------------------

/// Formats as "ip:port" (v4) or "[ip]:port" (v6), the conventional endpoint notation.
template <>
struct std::formatter<boost::corosio::endpoint>
{
   constexpr auto parse(std::format_parse_context& ctx) { return ctx.begin(); }

   template <typename FormatContext>
   auto format(const boost::corosio::endpoint& ep, FormatContext& ctx) const
   {
      if (ep.is_v6())
         return std::format_to(ctx.out(), "[{}]:{}", ep.v6_address(), ep.port());
      else
         return std::format_to(ctx.out(), "{}:{}", ep.v4_address(), ep.port());
   }
};

// =================================================================================================

/// Formats an nghttp2 header name/value pair as "name=value" ({}), or just the name ({:n}) or
/// value ({:v}) alone -- both are raw byte spans, not null-terminated.
template <>
struct std::formatter<nghttp2_nv>
{
   enum class part
   {
      name_and_value,
      name,
      value
   } what = part::name_and_value;

   constexpr auto parse(std::format_parse_context& ctx)
   {
      auto it = ctx.begin();
      if (it == ctx.end())
         return it;

      if (*it == 'n')
      {
         what = part::name;
         ++it;
      }
      else if (*it == 'v')
      {
         what = part::value;
         ++it;
      }

      if (it != ctx.end() && *it != '}')
         throw std::format_error("invalid format args for nghttp2_nv, expected 'n' or 'v'");

      return it;
   }

   auto format(const nghttp2_nv& nv, std::format_context& ctx) const
   {
      auto out = ctx.out();
      if (what == part::name || what == part::name_and_value)
         std::ranges::copy(std::views::counted(nv.name, nv.namelen), out);

      if (what == part::name_and_value)
         *out++ = '=';
      if (what == part::value || what == part::name_and_value)

         std::ranges::copy(std::views::counted(nv.value, nv.valuelen), out);

      return out;
   }
};

// =================================================================================================
