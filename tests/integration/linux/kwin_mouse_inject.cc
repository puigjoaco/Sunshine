/**
 * @file tests/integration/linux/kwin_mouse_inject.cc
 * @brief Exercise the production ordered pointer source only inside a private compositor.
 */
#include "src/platform/linux/kwin_text.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <thread>

/** @brief Replay global logical coordinates or delayed modifier/pointer chords from the harness. */
int main() {
  const char *runtime = std::getenv("XDG_RUNTIME_DIR");
  if (!runtime || std::string_view(runtime).find("/sunshine-mouse-") == std::string_view::npos) return 2;
  kwin::keyboard source;
  if (source.start() != kwin::text_result::sent) return 3;
  char type;
  double x, y;
  while (std::cin >> type >> x >> y) {
    auto result = kwin::text_result::failed;
    if (type == 'a' || type == 'r') result = source.motion(x, y, type == 'a');
    else if (type == 'b') result = source.button(static_cast<unsigned>(x), y != 0);
    else if (type == 'v' || type == 'h') result = source.scroll(static_cast<int>(x), type == 'h');
    else if (type == 'c' || type == 's') {
      result = source.key(0xA3, true);
      std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(x)));
      if (result != kwin::text_result::sent) return 4;
      if (type == 'c') {
        if (source.button(1, true) != kwin::text_result::sent) return 4;
        result = source.button(1, false);
      } else result = source.scroll(120, false);
      if (result != kwin::text_result::sent) return 4;
      std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(y)));
      result = source.key(0xA3, false);
    }
    if (result != kwin::text_result::sent && result != kwin::text_result::unavailable) return 5;
    std::cout << (result == kwin::text_result::unavailable ? "uinput" : "ok") << std::endl;
  }
  return 0;
}
