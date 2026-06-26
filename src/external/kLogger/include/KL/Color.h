#ifndef COLOR_H
#define COLOR_H

#include <string>

namespace KL {
namespace Color {
// ANSI escape codes.
inline constexpr const char* RED = "\033[31m";
inline constexpr const char* GREEN = "\033[32m";
inline constexpr const char* YELLOW = "\033[33m";
inline constexpr const char* RESET = "\033[0m";
}; // namespace Color
} // namespace KL

#endif //! COLOR_H