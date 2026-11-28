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

TEST(KwinVirtualOutputTest, AcceptsThePortaResizeLowerBound) {
  EXPECT_TRUE(kwin::virtual_mode(true, true, 256, 256, 60, 100));
  EXPECT_FALSE(kwin::virtual_mode(true, true, 255, 256, 60, 100));
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

TEST(KwinCaptureReconnectNestedTest, ResizesCaptureAndEncoderWithoutEndingSession) {
  const auto runtime = lizardbyte::common::get_env("XDG_RUNTIME_DIR");
  if (lizardbyte::common::get_env("SUNSHINE_TEST_KWIN_CAPTURE_RECONNECT") != "1" || runtime.find("/sunshine-exclusive-") == std::string::npos) {
    GTEST_SKIP() << "Requires the isolated nested-KWin test harness";
  }
  input::testing::set_platform_input({});
  const auto saved = config::video;
  const auto restore = util::fail_guard([&] {
    config::video = saved;
  });
  config::video.capture = "kwin";
  config::video.virtual_display = true;
  config::video.virtual_display_mode = "exclusive";
  config::video.virtual_display_scale = 100;
  config::video.encoder = "software";
  config::video.hevc_mode = 1;
  config::video.av1_mode = 1;
  const auto platform = platf::init();
  ASSERT_TRUE(platform);
  ASSERT_EQ(video::probe_encoders(), 0);
  ASSERT_TRUE(video::resize_supported());
  video::config_t client {};
  client.width = 1280;
  client.height = 720;
  client.framerate = 30;
  client.bitrate = 5000;
  client.slicesPerFrame = 1;
  client.numRefFrames = 1;
  client.resize = std::make_shared<stream_resize::state_t>(client.width, client.height);
  const auto mailbox = std::make_shared<safe::mail_raw_t>();
  const auto shutdown = mailbox->event<bool>(mail::shutdown);
  const auto packets = mail::man->queue<video::packet_t>(mail::video_packets);
  while (packets->peek()) {
    packets->pop();
  }
  std::jthread thread([&] {
    video::capture(mailbox, client, nullptr);
  });
  const auto stop = util::fail_guard([&] {
    kwin::exclusive_t::instance().session_stopping(false);
    shutdown->raise(true);
    if (thread.joinable()) {
      thread.join();
    }
  });
  auto next_frame = [&](int width, int height) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!shutdown->peek() && std::chrono::steady_clock::now() < deadline) {
      auto packet = packets->pop(std::chrono::milliseconds(100));
      if (!packet) {
        continue;
      }
      if (!packet->is_idr()) {
        continue;
      }
      const auto path = std::filesystem::path(runtime) / ("resize-idr-" + std::to_string(width) + "x" + std::to_string(height) + ".h264");
      {
        std::ofstream sample(path, std::ios::binary);
        sample.write(reinterpret_cast<const char *>(packet->data()), packet->data_size());
      }
      namespace bp = boost::process::v1;
      bp::ipstream output;
      bp::child probe(bp::search_path("ffprobe"), bp::args(std::vector<std::string> {"-v", "error", "-show_entries", "frame=key_frame,width,height", "-read_intervals", "%+#1", "-of", "json", path.string()}), bp::std_out > output, bp::std_err > bp::null);
      if (!probe.wait_for(std::chrono::seconds(2))) {
        probe.terminate();
        probe.wait();
        return false;
      }
      const auto decoded = nlohmann::json::parse(output, nullptr, false);
      if (probe.exit_code() == 0 && decoded.is_object() && decoded.contains("frames") && !decoded["frames"].empty()) {
        const auto &first = decoded["frames"][0];
        if (first.value("width", 0) == width && first.value("height", 0) == height && first.value("key_frame", 0) == 1) {
          return true;
        }
      }
    }
    return false;
  };
  ASSERT_TRUE(next_frame(1280, 720));
  auto owned = [] {
    auto names = platf::kwin_display_names();
    std::erase_if(names, [](const auto &name) {
      return !name.starts_with("Virtual-Sunshine-");
    });
    return names;
  };
  const auto names = owned();
  ASSERT_EQ(names.size(), 1);
  std::ofstream evidence(std::filesystem::path(runtime).parent_path() / "in-session-resize.log");
  for (const auto request : {stream_resize::request_t {1, 720, 1280}, {3, 1280, 720}, {4, 1440, 3120}}) {
    const auto started = std::chrono::steady_clock::now();
    if (request.sequence == 3) {
      client.resize->offer({2, 640, 480});
    }
    client.resize->offer(request);
    std::optional<stream_resize::result_t> result;
    while (!shutdown->peek() && std::chrono::steady_clock::now() - started < std::chrono::seconds(4)) {
      result = client.resize->take_result();
      if (result && result->mode.sequence == request.sequence) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_TRUE(result) << "Resize result timed out";
    ASSERT_EQ(result->mode.sequence, request.sequence);
    ASSERT_FALSE(result->failed);
    EXPECT_EQ(result->mode.width, request.width);
    EXPECT_EQ(result->mode.height, request.height);
    ASSERT_TRUE(next_frame(request.width, request.height));
    EXPECT_FALSE(shutdown->peek());
    EXPECT_EQ(owned(), names);
    const auto port = mailbox->event<input::touch_port_t>(mail::touch_port)->pop(std::chrono::milliseconds(100));
    ASSERT_TRUE(port);
    EXPECT_EQ(port->width, request.width);
    EXPECT_EQ(port->height, request.height);
    evidence << "IN_SESSION_RESIZE sequence=" << request.sequence << " size=" << request.width << 'x' << request.height
             << " IDR=1 elapsed_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count() << std::endl;
  }
  // Fail the first KScreen readback only after the requested mode actually
  // changed. The shim, output and sentinel all live in this private session.
  const auto original_path = lizardbyte::common::get_env("PATH");
  const auto real_doctor = boost::process::v1::search_path("kscreen-doctor").string();
  const auto shim_dir = std::filesystem::path(runtime) / "resize-shim";
  std::filesystem::create_directories(shim_dir);
  const auto shim = shim_dir / "kscreen-doctor";
  const auto sentinel = shim_dir / "fail-once";
  {
    std::ofstream script(shim);
    script << "#!/usr/bin/python3\nimport json, subprocess, sys\nfrom pathlib import Path\nreal=" << nlohmann::json(real_doctor).dump() << R"(
result=subprocess.run([real, *sys.argv[1:]], capture_output=True)
flag=Path(__file__).with_name('fail-once')
if result.returncode == 0 and sys.argv[1:] == ['-j'] and flag.exists():
    for output in json.loads(result.stdout)['outputs']:
        if not output['name'].startswith('Virtual-Sunshine-'): continue
        mode=next(m for m in output['modes'] if m['id'] == output['currentModeId'])
        if mode['size'] == {'width':1024, 'height':768}:
            flag.unlink()
            sys.exit(1)
sys.stdout.buffer.write(result.stdout)
sys.stderr.buffer.write(result.stderr)
sys.exit(result.returncode)
)";
    std::ofstream arm(sentinel);
  }
  std::filesystem::permissions(shim, std::filesystem::perms::owner_all);
  const auto restore_path = util::fail_guard([&] {
    setenv("PATH", original_path.c_str(), 1);
  });
  ASSERT_EQ(setenv("PATH", (shim_dir.string() + ":" + original_path).c_str(), 1), 0);
  const auto failure_started = std::chrono::steady_clock::now();
  client.resize->offer({5, 1024, 768});
  std::optional<stream_resize::result_t> rolled_back;
  while (std::chrono::steady_clock::now() - failure_started < std::chrono::seconds(4)) {
    rolled_back = client.resize->take_result();
    if (rolled_back) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_FALSE(std::filesystem::exists(sentinel)) << "Failure injection never reached the changed mode";
  ASSERT_TRUE(rolled_back);
  EXPECT_TRUE(rolled_back->failed);
  EXPECT_EQ(rolled_back->mode.sequence, 5);
  EXPECT_EQ(rolled_back->mode.width, 1440);
  EXPECT_EQ(rolled_back->mode.height, 3120);
  ASSERT_TRUE(next_frame(1440, 3120));
  EXPECT_EQ(owned(), names);
  EXPECT_FALSE(shutdown->peek());
  evidence << "IN_SESSION_ROLLBACK status=1 size=1440x3120 IDR=1 elapsed_ms="
           << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - failure_started).count() << std::endl;
  client.resize->offer({6, 257, 1280});
  const auto rejected = client.resize->take_result();
  ASSERT_TRUE(rejected);
  EXPECT_TRUE(rejected->failed);
  EXPECT_EQ(rejected->mode.width, 1440);
  EXPECT_EQ(rejected->mode.height, 3120);

  client.resize->offer({7, 720, 1280});
  const auto waiting = std::chrono::steady_clock::now();
  while (client.resize->needs_capture() && std::chrono::steady_clock::now() - waiting < std::chrono::seconds(1)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  const auto stopping = std::chrono::steady_clock::now();
  kwin::exclusive_t::instance().session_stopping(false);
  shutdown->raise(true);
  thread.join();
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - stopping);
  EXPECT_LT(elapsed, std::chrono::seconds(10));
  evidence << "IN_SESSION_DISCONNECT_DURING_RESIZE elapsed_ms=" << elapsed.count() << std::endl;
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
  if (name.empty() || runtime.find("/sunshine-exclusive-") == std::string::npos) {
    GTEST_SKIP();
  }
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
  if (name.empty() || runtime.find("/sunshine-exclusive-") == std::string::npos) {
    GTEST_SKIP();
  }
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
