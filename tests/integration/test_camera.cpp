/**
 * @file tests/integration/test_camera.cpp
 * @brief Opt-in real packet -> external H.264 decoder -> loopback -> ffmpeg capture check.
 */
#include <gtest/gtest.h>
#include <src/camera.h>
#ifdef __linux__
  #include <atomic>
  #include <cstdlib>
  #include <fcntl.h>
  #include <filesystem>
  #include <fstream>
  #include <mutex>
  #include <signal.h>
  #include <spawn.h>
  #include <sys/wait.h>
  #include <thread>
  #include <unistd.h>
extern char **environ;

namespace {
  constexpr size_t pixels = 1280 * 720, frame_size = pixels * 3 / 2;  ///< Fixed YUV420 output size.

  /** @brief Test-owned process with bounded automatic cleanup on every assertion path. */
  struct camera_process_t {
    pid_t pid = -1;  ///< Task-owned child.

    /** @brief Kill and reap this test process only. */
    ~camera_process_t() {
      stop();
    }

    /** @brief Stop and reap the process. */
    void stop() {
      if (pid <= 0) {
        return;
      }
      kill(pid, SIGKILL);
      while (waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {}
      std::cout << "Camera test reaped pid=" << pid << std::endl;
      pid = -1;
    }

    /**
     * @brief Start ffmpeg without a shell and record its PID/cwd.
     * @param args Full ffmpeg arguments.
     * @param output Optional stdout pipe.
     * @return True on successful spawn.
     */
    bool start(std::vector<std::string> args, int output = -1) {
      posix_spawn_file_actions_t actions;
      posix_spawn_file_actions_init(&actions);
      posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
      if (output >= 0) {
        posix_spawn_file_actions_adddup2(&actions, output, STDOUT_FILENO);
      } else {
        posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
      }
      std::vector<char *> argv;
      for (auto &a : args) {
        argv.push_back(a.data());
      }
      argv.push_back(nullptr);
      auto result = posix_spawn(&pid, "/usr/bin/ffmpeg", &actions, nullptr, argv.data(), environ);
      posix_spawn_file_actions_destroy(&actions);
      if (result) {
        pid = -1;
        return false;
      }
      std::cout << "Camera test spawned pid=" << pid << " cwd=" << std::filesystem::current_path() << std::endl;
      return true;
    }
  };

  /** @brief Build real camera envelopes and run the production parser before delivery. */
  void camera_feed(camera::receiver_t &receiver, const std::string &data, uint32_t seq, int rotation, bool omit = false) {
    size_t count = data.empty() ? 0 : (data.size() + camera::chunk_bytes - 1) / camera::chunk_bytes;
    for (size_t index = 0; index < std::max<size_t>(1, count); ++index) {
      if (omit && index == count / 2) {
        continue;
      }
      size_t size = data.empty() ? 0 : std::min(camera::chunk_bytes, data.size() - index * camera::chunk_bytes);
      std::string p(24, 0);
      p[0] = 1;
      p[1] = data.empty() ? 2 : seq % 30 == 0 ? 1 :
                                                0;
      auto put = [&](size_t offset, uint64_t value, size_t bytes) {
        for (size_t i = 0; i < bytes; ++i) {
          p[offset + i] = char(value >> (8 * i));
        }
      };
      put(2, rotation, 2);
      put(8, seq, 4);
      put(12, index, 2);
      put(14, count, 2);
      put(16, seq * 33333ull, 8);
      p.append(data, index * camera::chunk_bytes, size);
      auto parsed = camera::parse(p);
      ASSERT_TRUE(parsed);
      receiver.push(std::move(*parsed));
    }
  }

  /** @brief Find task-owned ffmpeg children to verify the receiver reaps them. */
  std::vector<int> camera_children() {
    std::vector<int> found;
    for (const auto &thread : std::filesystem::directory_iterator("/proc/self/task")) {
      std::ifstream children(thread.path() / "children");
      int pid;
      while (children >> pid) {
        std::ifstream comm("/proc/" + std::to_string(pid) + "/comm");
        std::string name;
        std::getline(comm, name);
        if (name == "ffmpeg") {
          found.push_back(pid);
        }
      }
    }
    return found;
  }
}  // namespace

TEST(CameraIntegration, RealH264IsUprightFreezesAfterLossAndReapsChildren) {
  if (!std::getenv("SUNSHINE_TEST_CAMERA_INTEGRATION")) {
    GTEST_SKIP() << "Requires an unused Sunshine Camera v4l2loopback device";
  }
  std::string device;
  std::error_code ec;
  for (const auto &entry : std::filesystem::directory_iterator("/sys/class/video4linux", ec)) {
    std::ifstream file(entry.path() / "name");
    std::string name;
    std::getline(file, name);
    if (name == "Sunshine Camera") {
      device = "/dev/" + entry.path().filename().string();
    }
  }
  ASSERT_FALSE(device.empty());
  // Fixtures stay in the scratch cwd, never in the repository.
  const std::string raw_path = "camera-fixture.yuv", h264_path = "camera-fixture.h264";

  struct fixtures_t {  ///< Remove generated fixtures even after a failed assertion.

    ~fixtures_t() {
      std::filesystem::remove("camera-fixture.yuv");
      std::filesystem::remove("camera-fixture.h264");
    }
  } fixtures;

  {
    std::ofstream output(raw_path, std::ios::binary);
    std::string raw(frame_size, char(128));
    for (int seq = 0; seq < 90; ++seq) {
      for (int y = 0; y < 720; ++y) {
        for (int x = 0; x < 1280; ++x) {
          raw[y * 1280 + x] = char(y < 360 ? (x < 640 ? 60 : 100) : (x < 640 ? 150 : 200));
        }
      }
      // A per-frame counter lets the captured output prove freeze/recovery and measure latency.
      for (int y = 64; y < 128; ++y) {
        for (int x = 64; x < 128; ++x) {
          raw[y * 1280 + x] = char(20 + seq * 2);
        }
      }
      // Changing texture makes P frames span multiple chunks; drop an interior chunk below.
      uint32_t noise = uint32_t(seq + 1);
      for (int y = 64; y < 128; ++y) {
        for (int x = 160; x < 288; ++x) {
          noise = noise * 1664525u + 1013904223u;
          raw[y * 1280 + x] = char(16 + ((noise >> 24) % 220));
        }
      }
      output.write(raw.data(), raw.size());
    }
    ASSERT_TRUE(output.good());
  }
  camera_process_t encoder;
  ASSERT_TRUE(encoder.start({"/usr/bin/ffmpeg", "-nostdin", "-y", "-loglevel", "error", "-f", "rawvideo", "-pix_fmt", "yuv420p", "-s", "1280x720", "-r", "30", "-i", raw_path, "-an", "-c:v", "libx264", "-threads", "1", "-preset", "ultrafast", "-tune", "zerolatency", "-g", "30", "-keyint_min", "30", "-sc_threshold", "0", "-x264-params", "aud=1:repeat-headers=1", "-f", "h264", h264_path}));
  int status = 0;
  ASSERT_EQ(waitpid(encoder.pid, &status, 0), encoder.pid);
  encoder.pid = -1;
  ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  std::ifstream encoded(h264_path, std::ios::binary);
  std::string all((std::istreambuf_iterator<char>(encoded)), {});
  std::vector<size_t> starts;
  for (size_t i = 0; i + 4 < all.size(); ++i) {
    size_t nal = 0;
    if (all[i] == 0 && all[i + 1] == 0 && all[i + 2] == 1) {
      nal = i + 3;
    } else if (all[i] == 0 && all[i + 1] == 0 && all[i + 2] == 0 && all[i + 3] == 1) {
      nal = i + 4;
    }
    if (nal && (uint8_t(all[nal]) & 31) == 9) {
      starts.push_back(i);
      i = nal;
    }
  }
  ASSERT_EQ(starts.size(), 90u);
  starts.push_back(all.size());
  std::vector<std::string> units;
  for (size_t i = 0; i < 90; ++i) {
    units.push_back(all.substr(starts[i], starts[i + 1] - starts[i]));
  }
  ASSERT_GT(units[10].size(), camera::chunk_bytes * 2);
  std::cout << "Camera loss fixture frame_bytes=" << units[10].size() << " chunks="
            << (units[10].size() + camera::chunk_bytes - 1) / camera::chunk_bytes << std::endl;
  for (int rotation : {0, 90}) {
    auto receiver = camera::create();
    ASSERT_NE(receiver, nullptr);
    EXPECT_EQ(camera::create(), nullptr);  // Single session owner.
    camera_feed(*receiver, units[0], 0, rotation);
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    camera_feed(*receiver, units[1], 1, rotation);
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    int fds[2];
    ASSERT_EQ(pipe2(fds, O_CLOEXEC), 0);
    camera_process_t recorder;
    ASSERT_TRUE(recorder.start({"/usr/bin/ffmpeg", "-nostdin", "-loglevel", "error", "-f", "v4l2", "-input_format", "yuv420p", "-video_size", "1280x720", "-i", device, "-an", "-threads", "1", "-c:v", "rawvideo", "-f", "rawvideo", "pipe:1"}, fds[1]));
    close(fds[1]);
    std::mutex mutex;
    std::string latest;
    size_t count = 0;
    std::atomic<int64_t> changed {0};
    std::thread reader([&] {
      std::string frame(frame_size, 0);
      size_t offset = 0;
      while (true) {
        auto bytes = read(fds[0], frame.data() + offset, frame.size() - offset);
        if (bytes < 0 && errno == EINTR) {
          continue;
        }
        if (bytes <= 0) {
          break;
        }
        offset += bytes;
        if (offset != frame.size()) {
          continue;
        }
        offset = 0;
        if (rotation == 0 && uint8_t(frame[96 * 1280 + 96]) >= 80 && changed.load() == 0) {
          changed = microphone::clock::now().time_since_epoch().count();
        }
        std::lock_guard lock(mutex);
        latest = frame;
        ++count;
      }
      close(fds[0]);
    });
    std::string frozen;
    microphone::clock::time_point recovery_sent;
    auto pacing = microphone::clock::now();
    for (int seq = 2; seq < 85; ++seq) {
      if (seq == 60) {
        recovery_sent = microphone::clock::now();
      }
      camera_feed(*receiver, units[seq], seq, rotation, seq == 10 || seq == 30);
      std::this_thread::sleep_until(pacing + std::chrono::milliseconds((seq - 1) * 34));
      if (seq == 21) {
        std::lock_guard lock(mutex);
        frozen = latest;
      }
      if (seq == 28 || seq == 58) {
        std::lock_guard lock(mutex);
        EXPECT_FALSE(frozen.empty());
        EXPECT_TRUE(latest == frozen) << "Loss must freeze the last good image until IDR";
      }
    }
    {
      std::lock_guard lock(mutex);
      EXPECT_EQ(latest.size(), frame_size);
      if (latest.size() == frame_size) {
        auto sample = [&](int x, int y) {
          return int(uint8_t(latest[y * 1280 + x]));
        };
        EXPECT_NEAR(sample(320, 180), rotation ? 150 : 60, 4);
        EXPECT_NEAR(sample(960, 180), rotation ? 60 : 100, 4);
        EXPECT_NEAR(sample(320, 540), rotation ? 200 : 150, 4);
        EXPECT_NEAR(sample(960, 540), rotation ? 100 : 200, 4);
        EXPECT_GT(count, 20u);
        std::cout << "Camera rotation=" << rotation << " size=1280x720 captured_frames=" << count << std::endl;
      }
    }
    if (rotation == 0) {
      EXPECT_GT(changed.load(), 0);
      double latency = std::chrono::duration<double, std::milli>(microphone::clock::duration(changed.load()) - recovery_sent.time_since_epoch()).count();
      std::cout << "Camera synthetic packet-to-device/capture latency_ms=" << latency << std::endl;
      EXPECT_GT(latency, 0);
      EXPECT_LT(latency, 250);
    }
    auto owned = camera_children();
    for (int pid : owned) {
      std::cout << "Camera live child pid=" << pid << " cwd=" << std::filesystem::current_path() << std::endl;
    }
    if (rotation == 0) {
      camera_feed(*receiver, {}, 85, rotation);
    } else {
      receiver.reset();  // Disconnect must black/reap even without a stop packet.
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    {
      std::lock_guard lock(mutex);
      if (latest.size() == frame_size) {
        EXPECT_NEAR(uint8_t(latest[180 * 1280 + 320]), 16, 2) << "Off must publish black";
      }
    }
    receiver.reset();
    recorder.stop();
    reader.join();
    for (int pid : owned) {
      EXPECT_EQ(kill(pid, 0), -1);
      EXPECT_EQ(errno, ESRCH);
    }
    EXPECT_TRUE(camera_children().empty());
  }
}
#endif
