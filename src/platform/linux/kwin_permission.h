/** @file src/platform/linux/kwin_permission.h
 * @brief Validate and upgrade the interfaces in generated KWin permission desktop files.
 */
#pragma once
#include <sstream>
#include <string>
#include <string_view>

namespace kwin {
  /** @brief Required protocols for screencast and exact Unicode input. */
  inline constexpr std::string_view required_interfaces[] = {"zkde_screencast_unstable_v1", "org_kde_kwin_fake_input"};

  /**
   * @brief Add missing permission tokens while preserving unrelated desktop-entry fields.
   * @param content Existing desktop-entry contents.
   * @return Contents with both required interface tokens present.
   */
  inline std::string upgrade_permission_interfaces(std::string_view content) {
    std::istringstream input {std::string {content}};
    std::string result, line;
    bool found = false;
    while (std::getline(input, line)) {
      if (line.starts_with("X-KDE-Wayland-Interfaces=")) {
        found = true;
        std::string tokens = line.substr(25);
        for (const auto required : required_interfaces) {
          std::istringstream list {tokens};
          std::string token;
          bool present = false;
          while (std::getline(list, token, ',')) {
            if (token == required) {
              present = true;
            }
          }
          if (!present) {
            if (!tokens.empty() && tokens.back() != ',') {
              tokens += ',';
            }
            tokens += required;
          }
        }
        line = "X-KDE-Wayland-Interfaces=" + tokens;
      }
      result += line + '\n';
    }
    if (!found) {
      result += "X-KDE-Wayland-Interfaces=zkde_screencast_unstable_v1,org_kde_kwin_fake_input\n";
    }
    return result;
  }

  /**
   * @brief Require both exact interface names, including in existing matching-Exec entries.
   * @param content Existing desktop-entry contents.
   * @return True when no permission-token upgrade is needed.
   */
  inline bool complete_permission_interfaces(std::string_view content) {
    return upgrade_permission_interfaces(content) == content;
  }
}  // namespace kwin
