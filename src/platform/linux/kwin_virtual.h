/**
 * @file src/platform/linux/kwin_virtual.h
 * @brief Pure validation of client-owned KWin virtual-output requests.
 */
#pragma once

// standard includes
#include <cmath>
#include <optional>

namespace kwin {
  /**
   * @brief Validated physical mode and compositor scale for an owned output.
   */
  struct virtual_mode_t {
    int width;  ///< Physical pixel width.
    int height;  ///< Physical pixel height.
    int refresh_mhz;  ///< Refresh rate in millihertz.
    double scale;  ///< Logical desktop scale.
  };

  /**
   * @brief Reject probes and invalid modes before sending a compositor request.
   * @param enabled Whether virtual display capture was explicitly enabled.
   * @param client_stream Whether this is a real stream rather than an encoder probe.
   * @param width Requested physical width.
   * @param height Requested physical height.
   * @param fps Requested rational refresh rate as a double.
   * @param scale_percent Requested logical scale percentage.
   * @return A bounded, validated request, or no value.
   */
  inline std::optional<virtual_mode_t> virtual_mode(bool enabled, bool client_stream, int width, int height, double fps, int scale_percent) {
    if (!enabled || !client_stream || width < 256 || height < 200 || width > 16384 || height > 16384 || !std::isfinite(fps) || fps < 1 || fps > 1000 || scale_percent < 100 || scale_percent > 400) {
      return std::nullopt;
    }
    return virtual_mode_t {width, height, static_cast<int>(std::lround(fps * 1000)), scale_percent / 100.0};
  }
}  // namespace kwin
