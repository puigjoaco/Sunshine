/**
 * @file tests/unit/platform/linux/test_kwin_virtual.cpp
 * @brief Validate native virtual modes and keep encoder probes side-effect free.
 */
// test includes
#include "../../../tests_common.h"

// standard includes
#include <limits>

// local includes
#include "src/platform/linux/kwin_virtual.h"

TEST(KwinVirtualOutputTest, EncoderProbeNeverCreatesAnOutput) {
  EXPECT_FALSE(kwin::virtual_mode(true, false, 1920, 1080, 60, 100));
  EXPECT_FALSE(kwin::virtual_mode(false, true, 3120, 1440, 60, 100));
}

TEST(KwinVirtualOutputTest, KeepsPhonePixelsIndependentOfLogicalScale) {
  const auto mode = kwin::virtual_mode(true, true, 3120, 1440, 60, 150);
  ASSERT_TRUE(mode);
  EXPECT_EQ(mode->width, 3120);
  EXPECT_EQ(mode->height, 1440);
  EXPECT_EQ(mode->refresh_mhz, 60000);
  EXPECT_DOUBLE_EQ(mode->scale, 1.5);
}

TEST(KwinVirtualOutputTest, SupportsLaptopAndFractionalRefreshRates) {
  const auto mode = kwin::virtual_mode(true, true, 2880, 1800, 59.94, 200);
  ASSERT_TRUE(mode);
  EXPECT_EQ(mode->width, 2880);
  EXPECT_EQ(mode->height, 1800);
  EXPECT_EQ(mode->refresh_mhz, 59940);
  EXPECT_DOUBLE_EQ(mode->scale, 2.0);
}

TEST(KwinVirtualOutputTest, RejectsUnboundedDimensionsRefreshAndScale) {
  EXPECT_FALSE(kwin::virtual_mode(true, true, 0, 1440, 60, 100));
  EXPECT_FALSE(kwin::virtual_mode(true, true, 3120, 0, 60, 100));
  EXPECT_FALSE(kwin::virtual_mode(true, true, 16385, 1440, 60, 100));
  EXPECT_FALSE(kwin::virtual_mode(true, true, 3120, 16385, 60, 100));
  EXPECT_FALSE(kwin::virtual_mode(true, true, 3120, 1440, 0, 100));
  EXPECT_FALSE(kwin::virtual_mode(true, true, 3120, 1440, 1001, 100));
  EXPECT_FALSE(kwin::virtual_mode(true, true, 3120, 1440, std::numeric_limits<double>::quiet_NaN(), 100));
  EXPECT_FALSE(kwin::virtual_mode(true, true, 3120, 1440, 60, 99));
  EXPECT_FALSE(kwin::virtual_mode(true, true, 3120, 1440, 60, 401));
}

#if defined(SUNSHINE_BUILD_KWIN) && defined(SUNSHINE_BUILD_CUDA)
  // lib includes
  #include <lizardbyte/common/env.h>

  // local includes
  #include "src/config.h"
  #include "src/platform/linux/cuda.h"
  #include "src/video.h"

namespace platf {
  std::shared_ptr<display_t> kwin_display(mem_type_e type, const std::string &name, const video::config_t &config);
}

namespace {
  /**
   * @brief Explicitly requested physical mode and scale for a live compositor test.
   */
  struct virtual_live_case_t {
    int width;  ///< Physical pixel width.
    int height;  ///< Physical pixel height.
    int fps;  ///< Integer refresh rate.
    int fps_x100;  ///< Optional fractional rate in hundredths of a hertz.
    int scale;  ///< Logical scale percentage.
  };

  /**
   * @brief Opt-in tests that temporarily create an owned KWin output.
   */
  class KwinVirtualOutputLiveTest: public ::testing::TestWithParam<virtual_live_case_t> {};
}  // namespace

TEST_P(KwinVirtualOutputLiveTest, CapturesNativeClientFrameOverPipeWire) {
  if (lizardbyte::common::get_env("SUNSHINE_TEST_KWIN_VIRTUAL") != "1") {
    GTEST_SKIP() << "Explicit opt-in required: creates a temporary virtual output in the live session";
  }
  const auto saved = config::video;
  const auto restore = util::fail_guard([&] {
    config::video = saved;
  });
  config::video.capture = "kwin";
  config::video.virtual_display = true;
  const auto &mode = GetParam();
  config::video.virtual_display_scale = mode.scale;
  const auto platform = platf::init();
  ASSERT_TRUE(platform);
  ASSERT_EQ(cuda::init(), 0);
  video::config_t client {};
  client.width = mode.width;
  client.height = mode.height;
  client.framerate = mode.fps;
  client.framerateX100 = mode.fps_x100;
  client.client_stream = true;
  auto display = platf::kwin_display(platf::mem_type_e::cuda, "", client);
  ASSERT_TRUE(display);
  EXPECT_EQ(display->width, mode.width);
  EXPECT_EQ(display->height, mode.height);
  EXPECT_EQ(display->logical_width, mode.width * 100 / mode.scale);
  EXPECT_EQ(display->logical_height, mode.height * 100 / mode.scale);
  bool cursor = true;
  bool got_frame = false;
  const auto status = display->capture(
    [&](std::shared_ptr<platf::img_t> &&image, bool captured) {
      got_frame = captured && image && image->width == mode.width && image->height == mode.height;
      return false;
    },
    [&](std::shared_ptr<platf::img_t> &image) {
      image = display->alloc_img();
      return image != nullptr;
    },
    &cursor
  );
  EXPECT_EQ(status, platf::capture_e::ok);
  EXPECT_TRUE(got_frame);
  display.reset();
}

INSTANTIATE_TEST_SUITE_P(
  NativeClients,
  KwinVirtualOutputLiveTest,
  ::testing::Values(virtual_live_case_t {3120, 1440, 60, 0, 100}, virtual_live_case_t {2880, 1800, 90, 0, 150}, virtual_live_case_t {2400, 1600, 60, 5994, 200})
);
#endif
