/**
 * @file tests/unit/platform/linux/test_misc.cpp
 * @brief Tests for Linux platform utility functions.
 */

#ifdef __linux__

  // standard includes
  #include <array>
  #include <cstdlib>
  #include <optional>
  #include <string>

  // lib includes
  #include <gtest/gtest.h>

  // local includes
  #include "src/config.h"
  #include "src/logging.h"
  #include "src/platform/linux/misc.h"
  #include "src/video.h"

namespace {
  /**
   * @brief Restore one environment variable when a test scope ends.
   */
  class environment_guard_t {
  public:
    /**
     * @brief Save the current value of an environment variable.
     *
     * @param name Environment variable name to restore.
     */
    explicit environment_guard_t(const char *name):
        name_ {name} {
      if (const char *value = std::getenv(name)) {
        value_ = value;
      }
    }

    /**
     * @brief Restore the saved value or remove the variable when it was originally absent.
     */
    ~environment_guard_t() {
      if (value_) {
        setenv(name_.c_str(), value_->c_str(), 1);
      } else {
        unsetenv(name_.c_str());
      }
    }

  private:
    std::string name_;  ///< Environment variable name.
    std::optional<std::string> value_;  ///< Value present before the test changed the environment.
  };

  constexpr std::array unsafe_environment_variables {
    "GDK_PIXBUF_MODULEDIR",
    "GDK_PIXBUF_MODULE_FILE",
    "GIO_EXTRA_MODULES",
    "GTK3_MODULES",
    "GTK_EXE_PREFIX",
    "GTK_IM_MODULE_FILE",
    "GTK_MODULES",
    "GTK_PATH",
    "QML2_IMPORT_PATH",
    "QML_IMPORT_PATH",
    "QT_PLUGIN_PATH",
    "QT_QPA_PLATFORM_PLUGIN_PATH",
  };

  /**
   * @brief Synchronize asynchronous test logging before changing the process environment.
   *
   * GoogleTest emits test-start records before fixture setup. Draining those records prevents the logging formatter
   * from reading environment-dependent state concurrently with `setenv()` or `unsetenv()` on older glibc releases.
   */
  class ProcessEnvironmentSecurity: public testing::Test {
  protected:
    /**
     * @brief Wait for pending test-start log records to finish formatting.
     */
    void SetUp() override {
      logging::log_flush();
    }
  };
}  // namespace

TEST_F(ProcessEnvironmentSecurity, RemovesModuleLoaders) {
  std::array<std::optional<environment_guard_t>, unsafe_environment_variables.size()> guards;
  for (std::size_t index = 0; index < unsafe_environment_variables.size(); ++index) {
    const auto *variable = unsafe_environment_variables[index];
    guards[index].emplace(variable);
    ASSERT_EQ(setenv(variable, "/tmp/untrusted-module", 1), 0);
  }

  ASSERT_TRUE(platf::sanitize_process_environment());
  for (const auto *variable : unsafe_environment_variables) {
    EXPECT_EQ(std::getenv(variable), nullptr) << variable;
  }
}

TEST_F(ProcessEnvironmentSecurity, PreservesTraySessionVariables) {
  environment_guard_t platform_theme_guard {"QT_QPA_PLATFORMTHEME"};
  environment_guard_t wayland_display_guard {"WAYLAND_DISPLAY"};
  ASSERT_EQ(setenv("QT_QPA_PLATFORMTHEME", "gtk3", 1), 0);
  ASSERT_EQ(setenv("WAYLAND_DISPLAY", "wayland-test", 1), 0);

  ASSERT_TRUE(platf::sanitize_process_environment());
  EXPECT_STREQ(std::getenv("QT_QPA_PLATFORMTHEME"), "gtk3");
  EXPECT_STREQ(std::getenv("WAYLAND_DISPLAY"), "wayland-test");
}

  #ifdef SUNSHINE_BUILD_DRM
TEST(KmsCapturePolicy, ExplicitKmsRemainsRequiredWithoutDiscoveredDisplays) {
  // Discovery may fail at the greeter or during an exclusive-output restore.
  // Both privilege retention and fatal startup handling must follow configuration.
  EXPECT_TRUE(platf::kms::capture_is_required("kms"));
}

TEST(KmsCapturePolicy, OtherBackendsPreserveOptionalStartupBehavior) {
  for (const auto *capture : {"", "nvfbc", "wlr", "x11", "portal", "kwin"}) {
    EXPECT_FALSE(platf::kms::capture_is_required(capture)) << capture;
  }
}

TEST(KmsCapturePolicy, SleepingConnectedOutputCanProbeWithoutScanout) {
  EXPECT_TRUE(platf::kms::can_probe_sleeping_output(true, true, 2560, 1440, false));
}

TEST(KmsCapturePolicy, SleepingOutputNeverSuppliesFakeClientCapture) {
  EXPECT_FALSE(platf::kms::can_probe_sleeping_output(true, true, 2560, 1440, true));
}

TEST(KmsCapturePolicy, MissingOrAwakeOutputsDoNotBypassCaptureValidation) {
  EXPECT_FALSE(platf::kms::can_probe_sleeping_output(false, true, 2560, 1440, false));
  EXPECT_FALSE(platf::kms::can_probe_sleeping_output(true, false, 2560, 1440, false));
  EXPECT_FALSE(platf::kms::can_probe_sleeping_output(true, true, 0, 1440, false));
  EXPECT_FALSE(platf::kms::can_probe_sleeping_output(true, true, 2560, 0, false));
  EXPECT_FALSE(platf::kms::can_probe_sleeping_output(true, true, -1, 1440, false));
  EXPECT_FALSE(platf::kms::can_probe_sleeping_output(true, true, 2560, -1, false));
}

    #ifdef SUNSHINE_BUILD_CUDA
TEST(KmsSleepingOutputLiveTest, ValidatesRealNvencWithoutScanout) {
  const auto *output = std::getenv("SUNSHINE_TEST_KMS_SLEEPING_OUTPUT");
  if (!output || !*output) {
    GTEST_SKIP() << "Requires an explicitly selected sleeping DRM output and CAP_SYS_ADMIN";
  }
  ASSERT_TRUE(platf::sanitize_process_environment());
  platf::kms::ensure_privileged_drm_worker_started();
  platf::drop_elevated_privileges(true);
  const auto saved = config::video;
  const auto restore = util::fail_guard([&] {
    config::video = saved;
  });
  config::video.capture = "kms";
  config::video.output_name = output;
  config::video.encoder = "nvenc";
  config::video.hevc_mode = 0;
  config::video.av1_mode = 0;
  const auto platform = platf::init();
  ASSERT_TRUE(platform);
  const auto names = platf::display_names(platf::mem_type_e::cuda);
  ASSERT_NE(std::ranges::find(names, output), names.end());
  ASSERT_EQ(video::probe_encoders(), 0);

  // With no session input allocated, the sleeping connector must never yield
  // a fake display to a streaming request even after a successful encode probe.
  video::config_t client {};
  client.width = 1920;
  client.height = 1080;
  client.framerate = 60;
  client.client_stream = true;
  EXPECT_FALSE(platf::display(platf::mem_type_e::cuda, output, client));

  if (const auto *wake = std::getenv("SUNSHINE_TEST_KMS_WAKE"); wake && std::string_view(wake) == "1") {
    // Exercise the same uinput mouse motion used before greeter capture, without
    // relying on a user-session KWin fake-input connection or a physical mouse.
    auto input = platf::input();
    ASSERT_TRUE(input);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    platf::move_mouse(input, 1, 1);
    platf::move_mouse(input, -1, -1);
    const auto display = platf::display(platf::mem_type_e::cuda, output, client);
    ASSERT_TRUE(display);
    bool cursor = false;
    bool captured = false;
    EXPECT_EQ(display->capture([&](std::shared_ptr<platf::img_t> &&image, bool valid) {
      captured = valid && image && image->width > 0 && image->height > 0;
      return false;
    },
                               [&](std::shared_ptr<platf::img_t> &image) {
                                 image = display->alloc_img();
                                 return image != nullptr;
                               },
                               &cursor),
              platf::capture_e::ok);
    EXPECT_TRUE(captured);
  }
}
    #endif
  #endif

#endif  // __linux__
