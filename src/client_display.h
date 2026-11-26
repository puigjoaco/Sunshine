/**
 * @file src/client_display.h
 * @brief Optional per-session virtual desktop scale hints.
 */
#pragma once

// standard includes
#include <charconv>
#include <string_view>

namespace client_display {
  /**
   * @brief Parse a strict integer scale percentage without failing a launch.
   * @param value Optional client value; empty means no hint.
   * @return A percentage in [100, 400], or zero for absent/invalid hints.
   */
  inline int parse_scale(std::string_view value) {
    if (value.empty()) {
      return 0;
    }
    int percent = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), percent);
    return error == std::errc {} && end == value.data() + value.size() && percent >= 100 && percent <= 400 ? percent : 0;
  }

  /**
   * @brief Select a client scale without changing the host configuration.
   * @param hint Parsed client scale, or zero when absent/invalid.
   * @param fallback Configured virtual-display percentage.
   * @return The bounded client percentage when present, otherwise fallback.
   */
  inline int select_scale(int hint, int fallback) {
    return hint >= 100 && hint <= 400 ? hint : fallback;
  }
}  // namespace client_display
