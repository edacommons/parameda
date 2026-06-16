#pragma once

#include <string_view>

namespace parameda {

inline constexpr int VERSION_MAJOR = 0;
inline constexpr int VERSION_MINOR = 1;
inline constexpr int VERSION_PATCH = 0;

// Matches the Python package version in pyproject.toml. Keep the
// numeric MAJOR/MINOR/PATCH constants and this string in sync.
inline constexpr std::string_view VERSION = "0.1.0";

// Returns the version string. Defined in version.cpp so the static
// library has at least one translation unit beyond headers.
std::string_view version() noexcept;

} // namespace parameda
