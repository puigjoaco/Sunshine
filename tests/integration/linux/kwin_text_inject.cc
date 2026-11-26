/**
 * @file tests/integration/linux/kwin_text_inject.cc
 * @brief Exercise the production text path only inside the private nested compositor.
 */
#include "src/platform/linux/kwin_text.h"

#include <cstdlib>
#include <iostream>
#include <string_view>

/** @brief Submit a complete UTF-8 argument through the production v6 keysym path. */
int main(int argc, char **argv) {
  const char *runtime = std::getenv("XDG_RUNTIME_DIR");
  if (!runtime || std::string_view {runtime}.find("/sunshine-text-") == std::string_view::npos || argc != 2) {
    return 2;
  }
  return kwin::type_text(argv[1]) == kwin::text_result::sent ? 0 : 3;
}
