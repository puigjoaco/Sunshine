/** @file tests/integration/linux/kwin_text_state.cc
 * @brief Observe modifiers or hold an independent keyboard only in the private KWin harness.
 */
#include "fake-input.h"
#include "keystate.h"
#include "src/platform/linux/kwin_text.h"

#include <array>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <wayland-client.h>

namespace {
  bool bind_input = true;
  org_kde_kwin_fake_input *input = nullptr;
  org_kde_kwin_keystate *keys = nullptr;
  std::array<unsigned, 8> states {};

  /** @brief Bind only the private compositor's test protocols. */
  void global(void *, wl_registry *registry, uint32_t id, const char *name, uint32_t version) {
    if (bind_input && !std::strcmp(name, "org_kde_kwin_fake_input")) {
      input = static_cast<org_kde_kwin_fake_input *>(wl_registry_bind(registry, id, &org_kde_kwin_fake_input_interface, 6));
    }
    if (!std::strcmp(name, "org_kde_kwin_keystate")) {
      keys = static_cast<org_kde_kwin_keystate *>(wl_registry_bind(registry, id, &org_kde_kwin_keystate_interface, version < 5 ? version : 5));
    }
  }

  /** @brief Ignore short-lived registry removals. */
  void removed(void *, wl_registry *, uint32_t) {}

  /** @brief Capture compositor modifier states, independent of the text injector. */
  void changed(void *, org_kde_kwin_keystate *, uint32_t key, uint32_t value) {
    if (key < states.size()) {
      states[key] = value;
    }
  }
}  // namespace

/** @brief Hold or observe an independent private keyboard; never connect to a shared input seat. */
int main(int argc, char **argv) {
  const char *runtime = std::getenv("XDG_RUNTIME_DIR");
  if (!runtime || std::string(runtime).find("/sunshine-text-") == std::string::npos || argc < 2) {
    return 2;
  }
  bind_input = std::string(argv[1]) != "state";
  auto *display = wl_display_connect(nullptr);
  if (!display) {
    return 3;
  }
  auto *registry = wl_display_get_registry(display);
  const wl_registry_listener listener {global, removed};
  wl_registry_add_listener(registry, &listener, nullptr);
  wl_display_roundtrip(display);
  if ((bind_input && !input) || !keys) {
    return 4;
  }
  const org_kde_kwin_keystate_listener keyListener {changed};
  org_kde_kwin_keystate_add_listener(keys, &keyListener, nullptr);
  if (input) {
    org_kde_kwin_fake_input_authenticate(input, "Sunshine private modifier test", "Validate external held keys without shared-seat input");
  }
  const std::string mode = argv[1];
  if (mode == "state") {
    org_kde_kwin_keystate_fetchStates(keys);
    wl_display_roundtrip(display);
    for (auto state : states) {
      std::cout << state << ' ';
    }
    std::cout << std::endl;
  } else if (argc == 4 && mode == "owned") {
    auto key = static_cast<unsigned>(std::stoul(argv[2]));
    kwin::keyboard source;
    unsigned vk = key == 42 ? 0xA0 : key == 54 ? 0xA1 : key == 29 ? 0xA2 :
                  key == 97 ? 0xA3 : key == 56 ? 0xA4 : key == 100 ? 0xA5 : 0x5B;
    if (source.key(vk, true) != kwin::text_result::sent) return 5;
    org_kde_kwin_keystate_fetchStates(keys);
    wl_display_roundtrip(display);
    const auto before = states;
    auto result = source.text(argv[3]);
    org_kde_kwin_keystate_fetchStates(keys);
    wl_display_roundtrip(display);
    const bool restored = states == before;
    source.key(vk, false);
    if (result != kwin::text_result::sent || !restored) {
      return 5;
    }
    std::cout << "owned modifier restored" << std::endl;
  } else if (argc == 3) {
    auto key = static_cast<unsigned>(std::stoul(argv[2]));
    org_kde_kwin_fake_input_keyboard_key(input, key, WL_KEYBOARD_KEY_STATE_PRESSED);
    wl_display_roundtrip(display);
    if (mode == "hold") {
      std::cout << "held" << std::endl;
      std::string command;
      std::getline(std::cin, command);
    }
    org_kde_kwin_fake_input_keyboard_key(input, key, WL_KEYBOARD_KEY_STATE_RELEASED);
    wl_display_roundtrip(display);
  }
  wl_display_disconnect(display);
  return 0;
}
