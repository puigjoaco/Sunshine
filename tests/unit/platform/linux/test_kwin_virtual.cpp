/**
 * @file tests/unit/platform/linux/test_kwin_virtual.cpp
 * @brief Validate native virtual modes and keep encoder probes side-effect free.
 */
// test includes
#include "../../../tests_common.h"

// standard includes
#include <fstream>
#include <limits>
#include <thread>
#include <utility>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
}

// local includes
#include "src/client_display.h"
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

TEST(KwinVirtualOutputTest, AcceptsPortraitWithoutSwappingOrCroppingPixels) {
  const auto mode = kwin::virtual_mode(true, true, 1440, 3120, 60, 100);
  ASSERT_TRUE(mode);
  EXPECT_EQ(mode->width, 1440);
  EXPECT_EQ(mode->height, 3120);
  EXPECT_EQ(mode->refresh_mhz, 60000);
  EXPECT_DOUBLE_EQ(mode->scale, 1);
}

#if defined(SUNSHINE_BUILD_KWIN)
  #include "src/config.h"
  #include "src/input.h"
  #include "src/platform/linux/kwin_exclusive.h"
  #include "src/video.h"

  #include <lizardbyte/common/env.h>
  #include <nlohmann/json.hpp>
  #include <signal.h>

namespace platf {
  std::shared_ptr<display_t> kwin_display(mem_type_e type, const std::string &name, const video::config_t &config);
  std::vector<std::string> kwin_display_names();
}  // namespace platf

TEST(KwinCaptureReconnectNestedTest, RetainsRealScreencastAndNegotiatesPortraitFrames) {
  const auto runtime = lizardbyte::common::get_env("XDG_RUNTIME_DIR");
  if (lizardbyte::common::get_env("SUNSHINE_TEST_KWIN_CAPTURE_RECONNECT") != "1" || runtime.find("/sunshine-exclusive-") == std::string::npos) {
    GTEST_SKIP() << "Requires the isolated nested-KWin test harness";
  }
  // No input backend is ever allocated in this separate test process. The
  // production cursor-position call therefore cannot reach a shared uinput seat.
  input::testing::set_platform_input({});
  const auto saved = config::video;
  const auto restore = util::fail_guard([&] {
    config::video = saved;
  });
  config::video.capture = "kwin";
  config::video.virtual_display = true;
  config::video.virtual_display_mode = "exclusive";
  config::video.virtual_display_scale = 100;
  config::video.virtual_display_restore_delay = 1;
  const auto platform = platf::init();
  ASSERT_TRUE(platform);
  video::config_t client {};
  client.width = 3120;
  client.height = 1440;
  client.framerate = 60;
  client.client_stream = true;
  auto owned_names = [] {
    auto names = platf::kwin_display_names();
    std::erase_if(names, [](const auto &name) {
      return !name.starts_with("Virtual-Sunshine-");
    });
    return names;
  };
  auto frame = [](const std::shared_ptr<platf::display_t> &display, int width, int height) {
    bool cursor = false;
    bool captured = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    const auto status = display->capture(
      [&](std::shared_ptr<platf::img_t> &&image, bool valid) {
        captured = valid && image && image->width == width && image->height == height;
        return !captured && std::chrono::steady_clock::now() < deadline;
      },
      [&](std::shared_ptr<platf::img_t> &image) {
        image = display->alloc_img();
        return image != nullptr;
      },
      &cursor
    );
    return std::pair {status, captured};
  };
  auto display = platf::kwin_display(platf::mem_type_e::system, "", client);
  ASSERT_TRUE(display);
  const auto first = frame(display, 3120, 1440);
  ASSERT_EQ(first.first, platf::capture_e::ok);
  ASSERT_TRUE(first.second);
  const auto landscape = owned_names();
  ASSERT_EQ(landscape.size(), 1);
  display.reset();
  client.width = 1440;
  client.height = 3120;
  client.virtual_display_scale = 250;
  display = platf::kwin_display(platf::mem_type_e::system, "", client);
  ASSERT_TRUE(display);
  auto portrait = frame(display, 1440, 3120);
  if (portrait.first == platf::capture_e::reinit) {
    // PipeWire can first announce the old format, then invalidate it when KWin
    // publishes the resized source. Follow the same reinit contract as video.cpp.
    display.reset();
    display = platf::kwin_display(platf::mem_type_e::system, "", client);
    ASSERT_TRUE(display);
    portrait = frame(display, 1440, 3120);
  }
  ASSERT_EQ(portrait.first, platf::capture_e::ok);
  ASSERT_TRUE(portrait.second);
  EXPECT_EQ(display->width, 1440);
  EXPECT_EQ(display->height, 3120);
  EXPECT_EQ(display->logical_width, 576);
  EXPECT_EQ(display->logical_height, 1248);
  EXPECT_EQ(config::video.virtual_display_scale, 100);
  EXPECT_EQ(owned_names(), landscape);
  display.reset();
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!owned_names().empty() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  EXPECT_TRUE(owned_names().empty());
}

TEST(KwinExclusiveNestedTest, ReconnectKeepsOutputAndFinalDisconnectExpires) {
  const auto name = lizardbyte::common::get_env("SUNSHINE_TEST_KWIN_EXCLUSIVE_OUTPUT");
  const auto runtime = lizardbyte::common::get_env("XDG_RUNTIME_DIR");
  if (name.empty() || runtime.find("/sunshine-exclusive-") == std::string::npos) {
    GTEST_SKIP() << "Requires the isolated nested-KWin test harness";
  }
  auto output = std::make_shared<int>(42);
  std::weak_ptr<int> lifetime = output;
  auto &lease = kwin::exclusive_t::instance();
  ASSERT_TRUE(lease.prepare(output.get()));
  ASSERT_TRUE(lease.begin(output.get(), name));
  lease.finish(std::move(output), 5);
  std::this_thread::sleep_for(std::chrono::milliseconds(250));
  EXPECT_FALSE(lifetime.expired());
  auto resumed = lease.resume();
  ASSERT_TRUE(resumed);
  ASSERT_EQ(resumed.get(), lifetime.lock().get());
  ASSERT_TRUE(lease.begin(resumed.get(), name));
  lease.finish(std::move(resumed), 1);
  std::this_thread::sleep_for(std::chrono::milliseconds(250));
  EXPECT_FALSE(lifetime.expired());
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!lifetime.expired() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  EXPECT_TRUE(lifetime.expired());
}

TEST(KwinExclusiveNestedTest, RecoversSavedDeskWhenGuardianIsKilled) {
  const auto name = lizardbyte::common::get_env("SUNSHINE_TEST_KWIN_EXCLUSIVE_OUTPUT");
  const auto runtime = lizardbyte::common::get_env("XDG_RUNTIME_DIR");
  if (name.empty() || runtime.find("/sunshine-exclusive-") == std::string::npos) {
    GTEST_SKIP() << "Requires the isolated nested-KWin test harness";
  }
  auto output = std::make_shared<int>(42);
  auto &lease = kwin::exclusive_t::instance();
  ASSERT_TRUE(lease.prepare(output.get()));
  ASSERT_TRUE(lease.begin(output.get(), name));
  const auto path = std::filesystem::path(lizardbyte::common::get_env("XDG_STATE_HOME")) / "sunshine/kwin-exclusive.json";
  std::ifstream snapshot(path);
  const auto state = nlohmann::json::parse(snapshot);
  ASSERT_EQ(kill(state.at("guardian_pid").get<int>(), SIGKILL), 0);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
  while (std::filesystem::exists(path) && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  EXPECT_FALSE(std::filesystem::exists(path));
  lease.finish(std::move(output), 0);
}

TEST(KwinExclusiveNestedTest, NormalSessionEndBypassesReconnectGrace) {
  const auto name = lizardbyte::common::get_env("SUNSHINE_TEST_KWIN_EXCLUSIVE_OUTPUT");
  const auto runtime = lizardbyte::common::get_env("XDG_RUNTIME_DIR");
  if (name.empty() || runtime.find("/sunshine-exclusive-") == std::string::npos) GTEST_SKIP();
  auto output = std::make_shared<int>(42);
  std::weak_ptr<int> lifetime = output;
  auto &lease = kwin::exclusive_t::instance();
  ASSERT_TRUE(lease.prepare(output.get()));
  ASSERT_TRUE(lease.begin(output.get(), name));
  lease.session_stopping(false);
  const auto started = std::chrono::steady_clock::now();
  lease.finish(std::move(output), 5);
  while (!lifetime.expired() && std::chrono::steady_clock::now() - started < std::chrono::seconds(3)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_TRUE(lifetime.expired());
  EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(3));
}

TEST(KwinExclusiveNestedTest, RotationSessionEndPreservesReconnectGrace) {
  const auto name = lizardbyte::common::get_env("SUNSHINE_TEST_KWIN_EXCLUSIVE_OUTPUT");
  const auto runtime = lizardbyte::common::get_env("XDG_RUNTIME_DIR");
  if (name.empty() || runtime.find("/sunshine-exclusive-") == std::string::npos) GTEST_SKIP();
  auto output = std::make_shared<int>(42);
  std::weak_ptr<int> lifetime = output;
  auto &lease = kwin::exclusive_t::instance();
  ASSERT_TRUE(lease.prepare(output.get()));
  ASSERT_TRUE(lease.begin(output.get(), name));
  lease.session_stopping(true);
  lease.finish(std::move(output), 5);
  EXPECT_FALSE(lifetime.expired());
  auto resumed = lease.resume();
  ASSERT_TRUE(resumed);
  ASSERT_TRUE(lease.begin(resumed.get(), name));
  lease.session_stopping(false);
  lease.finish(std::move(resumed), 5);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (!lifetime.expired() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_TRUE(lifetime.expired());
}

TEST(KwinExclusiveNestedTest, ActiveLeaseRestoresImmediatelyAtProcessExit) {
  const auto name = lizardbyte::common::get_env("SUNSHINE_TEST_KWIN_EXCLUSIVE_OUTPUT");
  const auto runtime = lizardbyte::common::get_env("XDG_RUNTIME_DIR");
  if (name.empty() || runtime.find("/sunshine-exclusive-") == std::string::npos) {
    GTEST_SKIP() << "Requires the isolated nested-KWin test harness";
  }
  auto output = std::make_shared<int>(42);
  auto &lease = kwin::exclusive_t::instance();
  ASSERT_TRUE(lease.prepare(output.get()));
  ASSERT_TRUE(lease.begin(output.get(), name));
  // Deliberately leave the lease active. The harness verifies restore after exit,
  // while its independent Wayland owner keeps the virtual output connected.
}

TEST(KwinPortraitEncoderTest, EncodesNativePortraitFrameWithoutADisplay) {
  if (lizardbyte::common::get_env("SUNSHINE_TEST_NVENC_PORTRAIT") != "1") {
    GTEST_SKIP() << "Explicit opt-in required for NVIDIA hardware encoding";
  }
  const auto *codec = avcodec_find_encoder_by_name("hevc_nvenc");
  ASSERT_NE(codec, nullptr);
  auto *context = avcodec_alloc_context3(codec);
  auto *frame = av_frame_alloc();
  auto *packet = av_packet_alloc();
  const auto cleanup = util::fail_guard([&] {
    av_packet_free(&packet);
    av_frame_free(&frame);
    avcodec_free_context(&context);
  });
  ASSERT_NE(context, nullptr);
  ASSERT_NE(frame, nullptr);
  ASSERT_NE(packet, nullptr);
  context->width = frame->width = 1440;
  context->height = frame->height = 3120;
  context->pix_fmt = AV_PIX_FMT_NV12;
  frame->format = AV_PIX_FMT_NV12;
  context->time_base = AVRational {1, 60};
  context->framerate = AVRational {60, 1};
  context->bit_rate = 20000000;
  ASSERT_GE(av_opt_set(context->priv_data, "preset", "p1", 0), 0);
  ASSERT_GE(av_opt_set(context->priv_data, "tune", "ull", 0), 0);
  ASSERT_GE(avcodec_open2(context, codec, nullptr), 0);
  ASSERT_GE(av_frame_get_buffer(frame, 32), 0);
  ptrdiff_t linesizes[4] {frame->linesize[0], frame->linesize[1], frame->linesize[2], frame->linesize[3]};
  ASSERT_GE(av_image_fill_black(frame->data, linesizes, AV_PIX_FMT_NV12, AVCOL_RANGE_MPEG, 1440, 3120), 0);
  ASSERT_GE(avcodec_send_frame(context, frame), 0);
  ASSERT_GE(avcodec_send_frame(context, nullptr), 0);
  ASSERT_GE(avcodec_receive_packet(context, packet), 0);
  EXPECT_GT(packet->size, 0);
  EXPECT_EQ(context->width, 1440);
  EXPECT_EQ(context->height, 3120);
}
#endif

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
  config::video.virtual_display_mode = "extend";  // Existing opt-in live tests never disable physical outputs.
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

TEST(KwinPortraitEncoderLiveTest, ConvertsNegotiatedCaptureAndEncodesAt120HzWithoutQueueing) {
  if (lizardbyte::common::get_env("SUNSHINE_TEST_KWIN_VIRTUAL") != "1") {
    GTEST_SKIP() << "Explicit opt-in required: creates a temporary virtual output in the live session";
  }
  const auto saved = config::video;
  const auto restore = util::fail_guard([&] {
    config::video = saved;
  });
  config::video.capture = "kwin";
  config::video.virtual_display = true;
  // Explicit DPMS verification also exercises the production exclusive path.
  config::video.virtual_display_mode = lizardbyte::common::get_env("SUNSHINE_TEST_KWIN_DPMS") == "1" ? "exclusive" : "extend";
  const auto platform = platf::init();
  ASSERT_TRUE(platform);
  ASSERT_EQ(cuda::init(), 0);
  video::config_t client {};
  client.width = 1440;
  client.height = 3120;
  client.framerate = 120;
  client.client_stream = true;
  auto display = platf::kwin_display(platf::mem_type_e::cuda, "", client);
  ASSERT_TRUE(display);
  auto device = display->make_avcodec_encode_device(platf::pix_fmt_e::nv12);
  ASSERT_TRUE(device);
  AVBufferRef *hardware = nullptr;
  AVBufferRef *frames = nullptr;
  auto *context = avcodec_alloc_context3(avcodec_find_encoder_by_name("hevc_nvenc"));
  auto *packet = av_packet_alloc();
  const auto cleanup = util::fail_guard([&] {
    device.reset();
    av_packet_free(&packet);
    avcodec_free_context(&context);
    av_buffer_unref(&frames);
    av_buffer_unref(&hardware);
  });
  ASSERT_TRUE(context);
  ASSERT_TRUE(packet);
  ASSERT_GE(av_hwdevice_ctx_create(&hardware, AV_HWDEVICE_TYPE_CUDA, nullptr, nullptr, 1), 0);
  frames = av_hwframe_ctx_alloc(hardware);
  ASSERT_TRUE(frames);
  auto *frame_context = reinterpret_cast<AVHWFramesContext *>(frames->data);
  frame_context->format = AV_PIX_FMT_CUDA;
  frame_context->sw_format = AV_PIX_FMT_NV12;
  frame_context->width = 1440;
  frame_context->height = 3120;
  device->init_hwframes(frame_context);
  ASSERT_GE(av_hwframe_ctx_init(frames), 0);
  context->width = 1440;
  context->height = 3120;
  context->pix_fmt = AV_PIX_FMT_CUDA;
  context->time_base = AVRational {1, 120};
  context->framerate = AVRational {120, 1};
  context->hw_frames_ctx = av_buffer_ref(frames);
  context->bit_rate = 47000000;
  context->max_b_frames = 0;
  ASSERT_GE(av_opt_set(context->priv_data, "preset", "p1", 0), 0);
  ASSERT_GE(av_opt_set(context->priv_data, "tune", "ull", 0), 0);
  ASSERT_GE(av_opt_set(context->priv_data, "delay", "0", 0), 0);
  ASSERT_GE(avcodec_open2(context, context->codec, nullptr), 0);
  auto *frame = av_frame_alloc();
  ASSERT_TRUE(frame);
  frame->width = 1440;
  frame->height = 3120;
  frame->format = AV_PIX_FMT_CUDA;
  ASSERT_EQ(device->set_frame(frame, frames), 0);  // Device takes ownership.
  bool cursor = true;
  bool encoded = false;
  EXPECT_EQ(display->capture([&](std::shared_ptr<platf::img_t> &&image, bool captured) {
    if (captured) {
      EXPECT_EQ(device->convert(*image), 0);
      EXPECT_GE(avcodec_send_frame(context, device->frame), 0);
      encoded = avcodec_receive_packet(context, packet) >= 0;
    }
    return false;
  },
                             [&](std::shared_ptr<platf::img_t> &image) {
                               image = display->alloc_img();
                               return image != nullptr;
                             },
                             &cursor),
            platf::capture_e::ok);
  EXPECT_TRUE(encoded);
  EXPECT_GT(packet->size, 0);
}

INSTANTIATE_TEST_SUITE_P(
  NativeClients,
  KwinVirtualOutputLiveTest,
  ::testing::Values(virtual_live_case_t {3120, 1440, 60, 0, 100}, virtual_live_case_t {1440, 3120, 60, 0, 100}, virtual_live_case_t {1440, 3120, 120, 0, 200}, virtual_live_case_t {2880, 1800, 90, 0, 150}, virtual_live_case_t {2400, 1600, 60, 5994, 200})
);
#endif

TEST(ClientDisplayScaleTest, ParsesOnlyBoundedIntegerPercentages) {
  for (const auto value : {"100", "125", "250", "400"}) {
    EXPECT_EQ(client_display::parse_scale(value), std::stoi(value));
  }
  for (const auto value : {"", "0", "99", "401", "-250", "+250", " 250", "250 ", "250x", "250.0", "999999999999999999999999"}) {
    EXPECT_EQ(client_display::parse_scale(value), 0) << value;
  }
}

TEST(ClientDisplayScaleTest, FallsBackWithoutLeakingBetweenClients) {
  EXPECT_EQ(client_display::select_scale(250, 100), 250);
  EXPECT_EQ(client_display::select_scale(0, 150), 150);
  EXPECT_EQ(client_display::select_scale(99, 150), 150);
  EXPECT_EQ(client_display::select_scale(401, 150), 150);
  const auto portrait = kwin::virtual_mode(true, true, 1440, 3120, 60, client_display::select_scale(275, 100));
  const auto landscape = kwin::virtual_mode(true, true, 3120, 1440, 60, client_display::select_scale(150, 100));
  ASSERT_TRUE(portrait);
  ASSERT_TRUE(landscape);
  EXPECT_DOUBLE_EQ(portrait->scale, 2.75);
  EXPECT_DOUBLE_EQ(landscape->scale, 1.5);
  EXPECT_DOUBLE_EQ(kwin::virtual_mode(true, true, 1920, 1080, 60, client_display::select_scale(0, 100))->scale, 1.0);
}
