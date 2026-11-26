/**
 * @file tests/integration/test_microphone.cpp
 * @brief Opt-in native PipeWire sine capture and default-source lifecycle check.
 */
#include <gtest/gtest.h>
#include <src/microphone.h>
#ifdef SUNSHINE_BUILD_MICROPHONE
  #include <array>
  #include <atomic>
  #include <cmath>
  #include <cstdio>
  #include <cstdlib>
  #include <fcntl.h>
  #include <fstream>
  #include <numbers>
  #include <opus/opus.h>
  #include <signal.h>
  #include <spawn.h>
  #include <sys/wait.h>
  #include <thread>
  #include <unistd.h>
  #include <vector>
extern char **environ;

TEST(MicrophoneIntegration, SyntheticOpusBecomesARecordableSine) {
  if (!std::getenv("SUNSHINE_TEST_MICROPHONE_INTEGRATION")) {
    GTEST_SKIP() << "Requires a local PipeWire/Pulse session; temporarily changes the default source";
  }
  int error;
  auto encoder = opus_encoder_create(48000, 1, OPUS_APPLICATION_VOIP, &error);
  ASSERT_NE(encoder, nullptr);
  opus_encoder_ctl(encoder, OPUS_SET_BITRATE(40000));
  auto receiver = microphone::create();
  ASSERT_NE(receiver, nullptr);
  std::array<int16_t, 960> samples {};
  std::array<unsigned char, 1024> encoded {};
  auto begin = microphone::clock::now();
  // Feed for 400 ms before attaching pw-record so discovery and default-source setup settle.
  auto feed = [&](int seq) {
    for (int i = 0; i < 960; ++i) {
      samples[i] = (seq >= 20 && seq < 50) ? 0 : 12000 * std::sin(2 * std::numbers::pi * 1000 * (seq * 960 + i) / 48000.0);
    }
    int size = opus_encode(encoder, samples.data(), 960, encoded.data(), encoded.size());
    EXPECT_GT(size, 0);
    receiver->push({true, uint32_t(seq), uint64_t(seq) * 960, std::string(reinterpret_cast<char *>(encoded.data()), size_t(size))});
  };
  for (int seq = 0; seq < 20; ++seq) {
    feed(seq);
    std::this_thread::sleep_until(begin + std::chrono::milliseconds(20 * (seq + 1)));
  }
  int output[2];
  ASSERT_EQ(pipe2(output, O_CLOEXEC), 0);
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, output[1], STDOUT_FILENO);
  char program[] = "pw-record", target[] = "--target=sunshine_microphone", rate[] = "--rate=48000", channels[] = "--channels=1", format[] = "--format=s16", raw[] = "--raw", dest[] = "-";
  char *args[] = {program, target, rate, channels, format, raw, dest, nullptr};
  pid_t recorder = -1;
  int spawned = posix_spawnp(&recorder, program, &actions, nullptr, args, environ);
  posix_spawn_file_actions_destroy(&actions);
  close(output[1]);
  std::vector<int16_t> recorded;
  std::atomic<int64_t> detected {0};
  std::thread reader([&] {
    std::array<int16_t, 480> chunk;
    size_t silence = 0;
    ssize_t bytes;
    while ((bytes = read(output[0], chunk.data(), sizeof(chunk))) > 0) {
      for (size_t i = 0; i < size_t(bytes) / sizeof(int16_t); ++i) {
        auto sample = chunk[i];
        if (std::abs(int(sample)) < 100) {
          ++silence;
        } else {
          if (silence > 4800 && detected.load() == 0) {
            detected = microphone::clock::now().time_since_epoch().count();
          }
          silence = 0;
        }
        recorded.push_back(sample);
      }
    }
    close(output[0]);
  });
  microphone::clock::time_point tone_sent;
  if (spawned == 0) {
    for (int seq = 20; seq < 120; ++seq) {
      if (seq == 80) {
        receiver.reset();
      }
      if (seq == 90) {
        receiver = microphone::create();
      }
      if (seq >= 80 && seq < 90) {
        std::this_thread::sleep_until(begin + std::chrono::milliseconds(20 * (seq + 1)));
        continue;
      }
      if (seq == 50) {
        tone_sent = microphone::clock::now();
      }
      feed(seq);
      std::this_thread::sleep_until(begin + std::chrono::milliseconds(20 * (seq + 1)));
    }
    kill(recorder, SIGINT);
    waitpid(recorder, nullptr, 0);
  }
  reader.join();
  receiver.reset();
  opus_encoder_destroy(encoder);
  ASSERT_EQ(spawned, 0);
  ASSERT_GT(recorded.size(), 48000u);
  double power = 0, real = 0, imag = 0;
  size_t last_half_second = recorded.size() - 24000;
  for (size_t i = last_half_second; i < recorded.size(); ++i) {
    power += double(recorded[i]) * recorded[i];
    real += recorded[i] * std::cos(2 * std::numbers::pi * 1000 * i / 48000.0);
    imag += recorded[i] * std::sin(2 * std::numbers::pi * 1000 * i / 48000.0);
  }
  auto count = recorded.size() - last_half_second;
  double rms = std::sqrt(power / count);
  double tone_fraction = 2 * (real * real + imag * imag) / (count * power);
  std::cout << "Microphone capture samples=" << recorded.size() << " RMS=" << rms << " 1kHz_power_fraction=" << tone_fraction << std::endl;
  ASSERT_GT(detected.load(), 0);
  auto latency = std::chrono::duration<double, std::milli>(microphone::clock::duration(detected.load()) - tone_sent.time_since_epoch()).count();
  std::cout << "Microphone synthetic packet-to-pw-record latency_ms=" << latency << std::endl;
  EXPECT_GT(latency, 0);
  EXPECT_LT(latency, 130);  // Leaves 20 ms for phone capture on LAN.
  EXPECT_GT(rms, 1000);
  EXPECT_GT(tone_fraction, 0.8);
}
#endif
