/**
 * @file tests/tests_main.cpp
 * @brief Entry point definition.
 */

// test includes
#include "src/microphone.h"
#include "tests_common.h"
#include "tests_environment.h"
#include "tests_events.h"

int main(int argc, char **argv) {
  if (argc == 3 && std::string_view(argv[1]) == "--microphone-guard") {
    return microphone::guard(std::string_view(argv[2]) == "3" ? 3 : -1);
  }
  testing::InitGoogleTest(&argc, argv);
  testing::AddGlobalTestEnvironment(new SunshineEnvironment);
  testing::UnitTest::GetInstance()->listeners().Append(new SunshineEventListener);
  return RUN_ALL_TESTS();
}
