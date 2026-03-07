/// include/kairos/core/version.hpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  kairos/core/version.hpp — Compile-time version information               ║
// ╚════════════════════════════════════════════════════════════════════════════╝
#pragma once

#include <string_view>

namespace kairos {

/// Kairos version string (set by CMake).
/// Falls back to "dev" if not built via CMake.
#ifndef KAIROS_VERSION
#define KAIROS_VERSION "dev"
#endif

constexpr std::string_view kVersion = KAIROS_VERSION;

/// Structured version components.
#ifndef KAIROS_VERSION_MAJOR
#define KAIROS_VERSION_MAJOR 0
#endif
#ifndef KAIROS_VERSION_MINOR
#define KAIROS_VERSION_MINOR 0
#endif
#ifndef KAIROS_VERSION_PATCH
#define KAIROS_VERSION_PATCH 0
#endif

constexpr int kVersionMajor = KAIROS_VERSION_MAJOR;
constexpr int kVersionMinor = KAIROS_VERSION_MINOR;
constexpr int kVersionPatch = KAIROS_VERSION_PATCH;

}  // namespace kairos
