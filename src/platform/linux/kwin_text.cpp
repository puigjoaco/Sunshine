/**
 * @file src/platform/linux/kwin_text.cpp
 * @brief Send Unicode without application-specific Ctrl+Shift+U sequences or layout changes.
 */
#include "kwin_text.h"

#include "fake-input.h"
#include "keystate.h"

#include <algorithm>
#include <array>
#include <linux/input-event-codes.h>
#include <chrono>
#include <cstring>
#include <gio/gio.h>
#include <memory>
#include <mutex>
#include <poll.h>
#include <set>
#include <sys/mman.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon.h>
#include <wayland-client.h>

namespace kwin {
  namespace {
    /** @brief Snapshot the layout group through KWin's session D-Bus API. */
    std::optional<std::uint32_t> current_layout(GDBusConnection *bus) {
      if (!bus) {
        return std::nullopt;
      }
      GVariant *reply = g_dbus_connection_call_sync(bus, "org.kde.keyboard", "/Layouts", "org.kde.KeyboardLayouts", "getLayout", nullptr, G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NO_AUTO_START, 2000, nullptr, nullptr);
      if (!reply) {
        return std::nullopt;
      }
      std::uint32_t group = 0;
      g_variant_get(reply, "(u)", &group);
      g_variant_unref(reply);
      return group;
    }

    /** @brief Restore the group after KWin's temporary keysym keymap has been removed. */
    bool restore_layout(GDBusConnection *bus, std::uint32_t group) {
      GVariant *reply = g_dbus_connection_call_sync(bus, "org.kde.keyboard", "/Layouts", "org.kde.KeyboardLayouts", "setLayout", g_variant_new("(u)", group), G_VARIANT_TYPE("(b)"), G_DBUS_CALL_FLAGS_NO_AUTO_START, 2000, nullptr, nullptr);
      if (!reply) {
        return false;
      }
      gboolean success = false;
      g_variant_get(reply, "(b)", &success);
      g_variant_unref(reply);
      return success;
    }

    /** @brief A private connection's protocol bindings and observed modifier states. */
    struct bindings {
      org_kde_kwin_fake_input *input = nullptr;
      org_kde_kwin_keystate *keys = nullptr;
      wl_seat *seat = nullptr;
      wl_keyboard *keyboard = nullptr;
      std::string keymap;
      unsigned states[8] {};
    };

    /** @brief Observe aggregate Caps/modifier state without reading physical input devices. */
    void key_state(void *data, org_kde_kwin_keystate *, std::uint32_t key, std::uint32_t state) {
      if (key < 8) {
        static_cast<bindings *>(data)->states[key] = state;
      }
    }

    /** @brief Retain the compositor's active keymap without reading input devices. */
    void keyboard_map(void *data, wl_keyboard *, std::uint32_t format, int fd, std::uint32_t size) {
      auto &bound = *static_cast<bindings *>(data);
      bound.keymap.clear();
      if (format == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 && size > 0) {
        void *map = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (map != MAP_FAILED) {
          bound.keymap.assign(static_cast<const char *>(map), strnlen(static_cast<const char *>(map), size));
          munmap(map, size);
        }
      }
      close(fd);
    }

    /** @brief Find the real Caps Lock position in the active layout; never guess a physical key. */
    std::optional<unsigned> caps_position(const std::string &text, unsigned layout) {
      std::unique_ptr<xkb_context, decltype(&xkb_context_unref)> context {xkb_context_new(XKB_CONTEXT_NO_FLAGS), xkb_context_unref};
      if (!context || text.empty()) return std::nullopt;
      std::unique_ptr<xkb_keymap, decltype(&xkb_keymap_unref)> map {xkb_keymap_new_from_string(context.get(), text.c_str(), XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS), xkb_keymap_unref};
      if (!map) return std::nullopt;
      for (auto key = xkb_keymap_min_keycode(map.get()); key <= xkb_keymap_max_keycode(map.get()); ++key) {
        const xkb_keysym_t *symbols = nullptr;
        if (key >= 8 && xkb_keymap_key_get_syms_by_level(map.get(), key, layout, 0, &symbols) == 1 && symbols[0] == XKB_KEY_Caps_Lock) return key - 8;
      }
      return std::nullopt;
    }

    /** @brief Discover the v6 interface; older versions cannot inject arbitrary Unicode. */
    void global(void *data, wl_registry *registry, std::uint32_t id, const char *interface, std::uint32_t version) {
      auto &bound = *static_cast<bindings *>(data);
      if (std::strcmp(interface, "org_kde_kwin_fake_input") == 0 && version >= 6) {
        bound.input = static_cast<org_kde_kwin_fake_input *>(wl_registry_bind(registry, id, &org_kde_kwin_fake_input_interface, 6));
      } else if (std::strcmp(interface, "wl_seat") == 0 && !bound.seat) {
        bound.seat = static_cast<wl_seat *>(wl_registry_bind(registry, id, &wl_seat_interface, 1));
        bound.keyboard = wl_seat_get_keyboard(bound.seat);
        static constexpr wl_keyboard_listener listener {
          keyboard_map,
          [](void *, wl_keyboard *, std::uint32_t, wl_surface *, wl_array *) {},
          [](void *, wl_keyboard *, std::uint32_t, wl_surface *) {},
          [](void *, wl_keyboard *, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t) {},
          [](void *, wl_keyboard *, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t) {},
          [](void *, wl_keyboard *, std::int32_t, std::int32_t) {}
        };
        wl_keyboard_add_listener(bound.keyboard, &listener, &bound);
      } else if (std::strcmp(interface, "org_kde_kwin_keystate") == 0) {
        bound.keys = static_cast<org_kde_kwin_keystate *>(wl_registry_bind(registry, id, &org_kde_kwin_keystate_interface, std::min(version, 5U)));
        static constexpr org_kde_kwin_keystate_listener listener {key_state};
        org_kde_kwin_keystate_add_listener(bound.keys, &listener, &bound);
      }
    }

    /** @brief Existing bindings remain usable until destroyed; socket failure is handled before switching. */
    void removed(void *, wl_registry *, std::uint32_t) {}

    /** @brief Mark a bounded display synchronization complete. */
    void synced(void *data, wl_callback *, std::uint32_t) {
      *static_cast<bool *>(data) = true;
    }

    /**
     * @brief Complete a display sync without an unbounded wait if the compositor stalls.
     * @param display Persistent connection shared by all keyboard and text events.
     * @return True when all preceding requests have been processed.
     */
    bool roundtrip(wl_display *display) {
      bool done = false;
      auto *callback = wl_display_sync(display);
      if (!callback) {
        return false;
      }
      static constexpr wl_callback_listener listener {synced};
      wl_callback_add_listener(callback, &listener, &done);
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
      bool success = true;
      while (!done) {
        if (wl_display_dispatch_pending(display) < 0) {
          success = false;
          break;
        }
        if (done) {
          break;
        }
        if (wl_display_prepare_read(display) != 0) {
          continue;
        }
        wl_display_flush(display);
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        pollfd fd {wl_display_get_fd(display), POLLIN, 0};
        if (remaining <= 0 || poll(&fd, 1, static_cast<int>(remaining)) <= 0 || !(fd.revents & POLLIN)) {
          wl_display_cancel_read(display);
          success = false;
          break;
        }
        if (wl_display_read_events(display) < 0) {
          success = false;
          break;
        }
      }
      wl_callback_destroy(callback);
      return success;
    }

    /** @brief Refresh the compositor state after an input-device write. */
    bool refresh(wl_display *display, bindings &bound) {
      org_kde_kwin_keystate_fetchStates(bound.keys);
      return roundtrip(display);
    }

    /** @brief Only Sunshine-owned modifiers are suspended around exact text injection. */
    bool modifier(unsigned key) {
      return key == KEY_LEFTSHIFT || key == KEY_RIGHTSHIFT || key == KEY_LEFTCTRL || key == KEY_RIGHTCTRL ||
             key == KEY_LEFTALT || key == KEY_RIGHTALT || key == KEY_LEFTMETA || key == KEY_RIGHTMETA;
    }
  }  // namespace

  int evdev_key(std::uint16_t key_code) {
    static constexpr std::array<std::pair<std::uint16_t, int>, 47> special_keys {{
      {0x08, KEY_BACKSPACE},
      {0x09, KEY_TAB},
      {0x0D, KEY_ENTER},
      {0x10, KEY_LEFTSHIFT},
      {0xA0, KEY_LEFTSHIFT},
      {0x11, KEY_LEFTCTRL},
      {0xA2, KEY_LEFTCTRL},
      {0x12, KEY_LEFTALT},
      {0xA4, KEY_LEFTALT},
      {0x14, KEY_CAPSLOCK},
      {0x1B, KEY_ESC},
      {0x20, KEY_SPACE},
      {0x21, KEY_PAGEUP},
      {0x22, KEY_PAGEDOWN},
      {0x23, KEY_END},
      {0x24, KEY_HOME},
      {0x25, KEY_LEFT},
      {0x26, KEY_UP},
      {0x27, KEY_RIGHT},
      {0x28, KEY_DOWN},
      {0x2C, KEY_SYSRQ},
      {0x2D, KEY_INSERT},
      {0x2E, KEY_DELETE},
      {0x5B, KEY_LEFTMETA},
      {0x5C, KEY_RIGHTMETA},
      {0x6A, KEY_KPASTERISK},
      {0x6B, KEY_KPPLUS},
      {0x6D, KEY_KPMINUS},
      {0x6E, KEY_KPDOT},
      {0x6F, KEY_KPSLASH},
      {0x90, KEY_NUMLOCK},
      {0x91, KEY_SCROLLLOCK},
      {0xA1, KEY_RIGHTSHIFT},
      {0xA3, KEY_RIGHTCTRL},
      {0xA5, KEY_RIGHTALT},
      {0xBA, KEY_SEMICOLON},
      {0xBB, KEY_EQUAL},
      {0xBC, KEY_COMMA},
      {0xBD, KEY_MINUS},
      {0xBE, KEY_DOT},
      {0xBF, KEY_SLASH},
      {0xC0, KEY_GRAVE},
      {0xDB, KEY_LEFTBRACE},
      {0xDC, KEY_BACKSLASH},
      {0xDD, KEY_RIGHTBRACE},
      {0xDE, KEY_APOSTROPHE},
      {0xE2, KEY_102ND},
    }};

    for (const auto &[vk, code] : special_keys) {
      if (vk == key_code) return code;
    }

    if (key_code >= 0x30 && key_code <= 0x39) {
      static constexpr std::array digit_keys {
        KEY_0,
        KEY_1,
        KEY_2,
        KEY_3,
        KEY_4,
        KEY_5,
        KEY_6,
        KEY_7,
        KEY_8,
        KEY_9,
      };
      return digit_keys[key_code - 0x30];
    }
    if (key_code >= 0x41 && key_code <= 0x5A) {
      static constexpr std::array letter_keys {
        KEY_A,
        KEY_B,
        KEY_C,
        KEY_D,
        KEY_E,
        KEY_F,
        KEY_G,
        KEY_H,
        KEY_I,
        KEY_J,
        KEY_K,
        KEY_L,
        KEY_M,
        KEY_N,
        KEY_O,
        KEY_P,
        KEY_Q,
        KEY_R,
        KEY_S,
        KEY_T,
        KEY_U,
        KEY_V,
        KEY_W,
        KEY_X,
        KEY_Y,
        KEY_Z,
      };
      return letter_keys[key_code - 0x41];
    }
    if (key_code >= 0x60 && key_code <= 0x69) {
      static constexpr std::array keypad_digit_keys {
        KEY_KP0,
        KEY_KP1,
        KEY_KP2,
        KEY_KP3,
        KEY_KP4,
        KEY_KP5,
        KEY_KP6,
        KEY_KP7,
        KEY_KP8,
        KEY_KP9,
      };
      return keypad_digit_keys[key_code - 0x60];
    }
    if (key_code >= 0x70 && key_code <= 0x87) {
      static constexpr std::array function_keys {
        KEY_F1,
        KEY_F2,
        KEY_F3,
        KEY_F4,
        KEY_F5,
        KEY_F6,
        KEY_F7,
        KEY_F8,
        KEY_F9,
        KEY_F10,
        KEY_F11,
        KEY_F12,
        KEY_F13,
        KEY_F14,
        KEY_F15,
        KEY_F16,
        KEY_F17,
        KEY_F18,
        KEY_F19,
        KEY_F20,
        KEY_F21,
        KEY_F22,
        KEY_F23,
        KEY_F24,
      };
      return function_keys[key_code - 0x70];
    }

    return -1;
  }


  /** @brief One selected Wayland device and its owned input state for the stream. */
  struct keyboard::impl {
    std::mutex mutex; ///< Serializes requests and their acknowledgments.
    wl_display *display = nullptr; ///< Private connection shared by keyboard and pointer requests.
    bindings bound; ///< Protocol objects and observed Caps Lock state.
    std::set<unsigned> held; ///< Evdev keys currently owned by this device.
    std::set<unsigned> buttons; ///< Evdev pointer buttons currently owned by this device.
    bool attempted = false; ///< Forbids reconnection or backend promotion within a stream.

    /** @brief Balance owned input without moving it to another source. */
    void release() {
      if (display && bound.input) {
        for (auto key : held) org_kde_kwin_fake_input_keyboard_key(bound.input, key, WL_KEYBOARD_KEY_STATE_RELEASED);
        for (auto button : buttons) org_kde_kwin_fake_input_button(bound.input, button, WL_POINTER_BUTTON_STATE_RELEASED);
        roundtrip(display);
      }
      held.clear();
      buttons.clear();
    }
    /** @brief Release the old source before disposing it, including during a source failure. */
    void reset() {
      release();
      if (bound.input) org_kde_kwin_fake_input_destroy(bound.input);
      if (bound.keys) org_kde_kwin_keystate_destroy(bound.keys);
      if (bound.keyboard) wl_keyboard_destroy(bound.keyboard);
      if (bound.seat) wl_seat_destroy(bound.seat);
      if (display) wl_display_disconnect(display);
      display = nullptr;
      bound = {};
    }

    /**
     * @brief Discover or acknowledge this same connection; never replay a possibly submitted event.
     * @return Sent on acknowledgment, unavailable on initial discovery failure, otherwise failed.
     */
    text_result connect() {
      if (display) {
        if (roundtrip(display)) return text_result::sent;
        reset();
        return text_result::failed;
      }
      if (attempted) return text_result::failed; // Pinned for the stream, even after compositor loss.
      attempted = true;
      display = wl_display_connect(nullptr);
      if (!display) return text_result::unavailable;
      auto *registry = wl_display_get_registry(display);
      static constexpr wl_registry_listener listener {global, removed};
      wl_registry_add_listener(registry, &listener, &bound);
      bool found = roundtrip(display);
      wl_registry_destroy(registry);
      if (!found || !bound.input || !bound.keys) {
        reset();
        return text_result::unavailable;
      }
      org_kde_kwin_fake_input_authenticate(bound.input, "Sunshine", "Remote keyboard and text");
      if (!roundtrip(display)) {
        reset();
        return text_result::failed;
      }
      return text_result::sent;
    }
    /**
     * @brief Queue one key request on the selected device.
     * @param key Evdev key position.
     * @param down True for press, false for release.
     */
    void stroke(unsigned key, bool down) {
      org_kde_kwin_fake_input_keyboard_key(bound.input, key, down ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED);
    }
  };

  keyboard::keyboard(): state {std::make_unique<impl>()} {}
  keyboard::~keyboard() { state->reset(); }

  text_result keyboard::start() {
    const std::lock_guard lock {state->mutex};
    return state->connect();
  }

  void keyboard::release_all() {
    const std::lock_guard lock {state->mutex};
    state->release();
  }

  text_result keyboard::key(std::uint16_t virtual_key, bool pressed) {
    const std::lock_guard lock {state->mutex};
    const auto connected = state->connect();
    if (connected != text_result::sent) return connected;
    const int code = evdev_key(virtual_key);
    if (code < 0) return text_result::failed;
    state->stroke(code, pressed);
    if (pressed) state->held.insert(code);
    else state->held.erase(code);
    // Acknowledges the actual key request, including releases, before the next event.
    if (roundtrip(state->display)) return text_result::sent;
    state->reset();
    return text_result::failed;
  }

  text_result keyboard::motion(double x, double y, bool absolute) {
    const std::lock_guard lock {state->mutex};
    const auto connected = state->connect();
    if (connected != text_result::sent) return connected;
    if (absolute) org_kde_kwin_fake_input_pointer_motion_absolute(state->bound.input, wl_fixed_from_double(x), wl_fixed_from_double(y));
    else org_kde_kwin_fake_input_pointer_motion(state->bound.input, wl_fixed_from_double(x), wl_fixed_from_double(y));
    if (roundtrip(state->display)) return text_result::sent;
    state->reset();
    return text_result::failed;
  }

  text_result keyboard::button(unsigned button, bool pressed) {
    static constexpr unsigned codes[] {0, BTN_LEFT, BTN_MIDDLE, BTN_RIGHT, BTN_SIDE, BTN_EXTRA};
    if (button == 0 || button >= std::size(codes)) return text_result::failed;
    const std::lock_guard lock {state->mutex};
    const auto connected = state->connect();
    if (connected != text_result::sent) return connected;
    const auto code = codes[button];
    org_kde_kwin_fake_input_button(state->bound.input, code, pressed ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED);
    if (pressed) state->buttons.insert(code);
    else state->buttons.erase(code);
    if (roundtrip(state->display)) return text_result::sent;
    state->reset();
    return text_result::failed;
  }

  text_result keyboard::scroll(int distance, bool horizontal) {
    const std::lock_guard lock {state->mutex};
    // The roundtrip acknowledges prior keys, motion and buttons before a plain uinput scroll.
    const auto connected = state->connect();
    if (connected != text_result::sent) return connected;
    if (std::none_of(state->held.begin(), state->held.end(), modifier)) return text_result::unavailable;
    // Modified scrolling must share modifier ordering. fake_input has no wheel/value120 metadata;
    // continuous axis trades that precision for correct Ctrl/Shift scrolling. Plain scroll stays uinput.
    const double value = (horizontal ? distance : -static_cast<double>(distance)) / 12.0;
    org_kde_kwin_fake_input_axis(state->bound.input, horizontal ? WL_POINTER_AXIS_HORIZONTAL_SCROLL : WL_POINTER_AXIS_VERTICAL_SCROLL, wl_fixed_from_double(value));
    if (roundtrip(state->display)) return text_result::sent;
    state->reset();
    return text_result::failed;
  }

  text_result keyboard::text(std::string_view text) {
    const auto keysyms = text_keysyms(text);
    if (!keysyms) return text_result::failed;
    if (keysyms->empty()) return text_result::sent;
    const std::lock_guard lock {state->mutex};
    const auto connected = state->connect();
    if (connected != text_result::sent) return connected;
    auto *display = state->display;
    auto &bound = state->bound;
    std::unique_ptr<GDBusConnection, decltype(&g_object_unref)> bus {g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr), g_object_unref};
    const auto layout = current_layout(bus.get());
    if (!layout || !refresh(display, bound)) return text_result::failed;
    const auto capsKey = caps_position(bound.keymap, *layout);
    const bool caps = bound.states[0] == 2 && capsKey.has_value();
    // Releases, text and re-presses all use this device and this socket. Right Alt (AltGr)
    // needs no aggregate-state mask: its actual key is released just like every other modifier.
    for (auto key : state->held) if (modifier(key)) state->stroke(key, false);
    auto toggle_caps = [&] {
      state->stroke(*capsKey, true);
      state->stroke(*capsKey, false);
    };
    if (caps) toggle_caps();
    // Physical modifiers held at the PC remain an accepted limitation: KWin cannot
    // release/restore another device's keys. Never read devices or fake replacement keys.
    for (const auto keysym : *keysyms) {
      org_kde_kwin_fake_input_keyboard_keysym(bound.input, keysym, WL_KEYBOARD_KEY_STATE_PRESSED);
      org_kde_kwin_fake_input_keyboard_keysym(bound.input, keysym, WL_KEYBOARD_KEY_STATE_RELEASED);
    }
    const bool sent = roundtrip(display);
    if (caps) toggle_caps();
    const bool capsRestored = roundtrip(display);
    const bool restored = restore_layout(bus.get(), *layout);
    // A scratch keymap releases ordinary held keys too. Restore the entire owned pressed set.
    for (auto key : state->held) state->stroke(key, true);
    const bool keysRestored = roundtrip(display);
    if (!sent || !capsRestored || !keysRestored) state->reset();
    return sent && capsRestored && restored && keysRestored ? text_result::sent : text_result::failed;
  }

  text_result type_text(std::string_view text) {
    keyboard source;
    return source.text(text);
  }
}  // namespace kwin
