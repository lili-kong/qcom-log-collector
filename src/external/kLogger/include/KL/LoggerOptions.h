// LoggerOptions.h  ---------------------------------------------------------
#pragma once
#include <cstdint>

namespace KL {

enum class LogOption : std::uint32_t
{
   None = 0,
   DebugToFile = 1u << 0, // write DEBUG messages to the file sink
};

inline LogOption operator|(LogOption lhs, LogOption rhs) noexcept
{
   return static_cast<LogOption>(static_cast<std::uint32_t>(lhs) | static_cast<std::uint32_t>(rhs));
}
inline LogOption operator&(LogOption lhs, LogOption rhs) noexcept
{
   return static_cast<LogOption>(static_cast<std::uint32_t>(lhs) & static_cast<std::uint32_t>(rhs));
}
inline LogOption& operator|=(LogOption& lhs, LogOption rhs) noexcept
{
   lhs = lhs | rhs;
   return lhs;
}
inline LogOption& operator&=(LogOption& lhs, LogOption rhs) noexcept
{
   lhs = lhs & rhs;
   return lhs;
}
inline LogOption operator~(LogOption opt) noexcept
{
   return static_cast<LogOption>(~static_cast<std::uint32_t>(opt));
}
inline bool any(LogOption opt) noexcept
{
   return static_cast<std::uint32_t>(opt) != 0;
}

} // namespace KL