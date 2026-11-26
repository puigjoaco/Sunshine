/** @file tests/integration/linux/kwin_text_order.cc
 * @brief Stress the production shared source with delayed keys and interleaved text.
 */
#include "src/platform/linux/kwin_text.h"

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <functional>
#include <iostream>
#include <mutex>
#include <string_view>
#include <string>
#include <thread>

/** @brief Queue arrival-order input, with keyboard delays longer than the removed timing guard. */
int main(int argc, char **argv) {
  const char *runtime = std::getenv("XDG_RUNTIME_DIR");
  if (!runtime || std::string_view(runtime).find("/sunshine-text-") == std::string_view::npos) return 2;
  if (argc == 2 && std::string_view(argv[1]) == "release") {
    kwin::keyboard source;
    return source.key(0xA3, true) == kwin::text_result::sent ? 0 : 3;
    // Destructor must release a held Ctrl even without a matching incoming key-up.
  }
  if (argc == 2 && std::string_view(argv[1]) == "repeat") {
    kwin::keyboard source;
    if (source.key(0x58, true) != kwin::text_result::sent) return 3;
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    return source.key(0x58, false) == kwin::text_result::sent ? 0 : 3;
  }
  if (argc == 2 && std::string_view(argv[1]) == "held") {
    kwin::keyboard source;
    if (source.key(0x58, true) != kwin::text_result::sent || source.text("😀") != kwin::text_result::sent) return 3;
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    return source.key(0x58, false) == kwin::text_result::sent ? 0 : 3;
  }
  if (argc == 2 && std::string_view(argv[1]) == "pin") {
    const std::string original = std::getenv("WAYLAND_DISPLAY");
    setenv("WAYLAND_DISPLAY", "absent-private-compositor", 1);
    kwin::keyboard unavailable;
    if (unavailable.start() != kwin::text_result::unavailable) return 3;
    setenv("WAYLAND_DISPLAY", original.c_str(), 1);
    if (unavailable.start() != kwin::text_result::failed) return 3;
    kwin::keyboard next_stream;
    return next_stream.start() == kwin::text_result::sent ? 0 : 3;
  }
  std::deque<std::function<kwin::text_result(kwin::keyboard &)>> events;
  auto key = [&](unsigned vk, bool down) {
    events.push_back([=](kwin::keyboard &source) {
      return source.key(vk, down);
    });
  };
  for (unsigned i = 0; i < 100; i++) {
    key(0x41, true); key(0x41, false);
    if (i % 10 == 0) { key(0xA3, true); key(0xA3, false); }
    events.push_back([](kwin::keyboard &source) { return source.text("ñ"); });
    key(0x4F, true); key(0x4F, false); key(0x0D, true); key(0x0D, false);
  }
  kwin::keyboard source;
  unsigned sequence = 0;
  for (auto &event : events) {
    // Delays occur at the injection boundary, after arrival order has been established.
    std::this_thread::sleep_for(std::chrono::milliseconds(sequence++ % 97 == 0 ? 80 : 1 + sequence % 3));
    if (event(source) != kwin::text_result::sent) return 3;
  }
  std::cout << "100 mixed lines acknowledged on one source, including 80ms delays" << std::endl;
  return 0;
}
