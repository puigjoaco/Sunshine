/**
 * @file src/platform/linux/kwin_text.h
 * @brief Layout-independent Unicode injection through KWin's privileged keysym protocol.
 */
#pragma once

#include <memory>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace kwin {
  /** @brief Result distinguishes an unsupported compositor from a possibly partial injection. */
  enum class text_result {
    unavailable,  ///< No usable privileged keysym interface; legacy fallback is permitted.
    sent,  ///< Complete text submitted without modifier chords or implicit Return.
    failed,  ///< Invalid text or connection failure; do not retry through another backend.
  };

  /**
   * @brief Decode strict UTF-8 into XKB Unicode keysyms without changing the user's layout.
   * @param text UTF-8 text; newline and tab are explicit key requests.
   * @return Keysyms, or no value for malformed UTF-8 or unsupported control characters.
   */
  inline std::optional<std::vector<std::uint32_t>> text_keysyms(std::string_view text) {
    std::vector<std::uint32_t> result;
    for (std::size_t i = 0; i < text.size();) {
      const auto first = static_cast<unsigned char>(text[i++]);
      std::uint32_t cp = first;
      unsigned remaining = 0;
      std::uint32_t minimum = 0;
      if (first >= 0xF0 && first <= 0xF4) {
        cp &= 7;
        remaining = 3;
        minimum = 0x10000;
      } else if (first >= 0xE0 && first <= 0xEF) {
        cp &= 15;
        remaining = 2;
        minimum = 0x800;
      } else if (first >= 0xC2 && first <= 0xDF) {
        cp &= 31;
        remaining = 1;
        minimum = 0x80;
      } else if (first >= 0x80) {
        return std::nullopt;
      }
      if (i + remaining > text.size()) {
        return std::nullopt;
      }
      while (remaining--) {
        const auto next = static_cast<unsigned char>(text[i++]);
        if ((next & 0xC0) != 0x80) {
          return std::nullopt;
        }
        cp = (cp << 6) | (next & 63);
      }
      if (cp < minimum || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        return std::nullopt;
      }
      if (cp == '\n' || cp == '\r') {
        result.push_back(0xFF0D);  // An explicit newline, never an implicit sequence terminator.
      } else if (cp == '\t') {
        result.push_back(0xFF09);
      } else if (cp < 0x20 || (cp >= 0x7F && cp < 0xA0)) {
        return std::nullopt;
      } else {
        result.push_back(cp <= 0xFF ? cp : (0x01000000 | cp));
      }
    }
    return result;
  }

  /**
   * @brief Map a Windows virtual key to the evdev position used by the fallback backend.
   * @param key_code Windows virtual-key code without packet flag bits.
   * @return Evdev key code, or -1 for an unsupported virtual key.
   */
  int evdev_key(std::uint16_t key_code);

  /** @brief One stream's ordered keyboard and pointer connection; never promotes a failed source. */
  class keyboard {
  public:
    /** @brief Construct an unselected source; start() chooses its connection once. */
    keyboard();
    /** @brief Release all owned keys/buttons and destroy the private connection. */
    ~keyboard();
    /** @brief Connections cannot be copied between streams. */
    keyboard(const keyboard &) = delete;
    /** @brief Connections cannot be assigned between streams. */
    keyboard &operator=(const keyboard &) = delete;
    /**
     * @brief Select and acknowledge the source once at stream startup.
     * @return Sent if usable, unavailable if absent, or failed after a connection error.
     */
    text_result start();
    /**
     * @brief Submit and acknowledge one key transition in arrival order.
     * @param virtual_key Windows virtual-key code.
     * @param pressed True for key-down, false for key-up.
     * @return Sent on acknowledgment; failed events must not be replayed on another backend.
     */
    text_result key(std::uint16_t virtual_key, bool pressed);
    /**
     * @brief Inject exact UTF-8 text, restoring the layout and all owned held keys afterwards.
     * @param utf8 Committed text; only explicit newlines generate Return.
     * @return Sent on acknowledgment, or failed without replaying a possibly partial commit.
     */
    text_result text(std::string_view utf8);
    /**
     * @brief Submit logical pointer motion on the same connection as keyboard events.
     * @param x Global logical X position, or relative X delta.
     * @param y Global logical Y position, or relative Y delta.
     * @param absolute True for global positioning, false for relative movement.
     * @return Sent on acknowledgment, or failed without changing input sources.
     */
    text_result motion(double x, double y, bool absolute);
    /**
     * @brief Submit a mouse button on the same connection as keyboard events.
     * @param button Moonlight mouse button: left=1, middle=2, right=3, side=4, extra=5.
     * @param pressed True for button-down, false for button-up.
     * @return Sent on acknowledgment, or failed without changing input sources.
     */
    text_result button(unsigned button, bool pressed);
    /**
     * @brief Acknowledge prior input, using continuous axis only when an owned modifier is held.
     * @param distance Signed wheel distance in 1/120-detent units.
     * @param horizontal True for horizontal scrolling, false for vertical scrolling.
     * @return Sent for modified scrolling; unavailable tells the caller to preserve uinput value120.
     */
    text_result scroll(int distance, bool horizontal);
    /** @brief Release and acknowledge every owned key/button while keeping the selected connection. */
    void release_all();

  private:
    struct impl;
    std::unique_ptr<impl> state;
  };

  /**
   * @brief Standalone text helper for isolated tests; production uses the stream's shared source.
   * @param text UTF-8 text for the private test compositor.
   * @return Injection result; failed commits must not be retried through a different backend.
   */
  text_result type_text(std::string_view text);
}  // namespace kwin
