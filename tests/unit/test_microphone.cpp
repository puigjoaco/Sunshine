/**
 * @file tests/unit/test_microphone.cpp
 * @brief Packet validation, replay handling and deterministic jitter/PLC scheduling.
 */
#include <gtest/gtest.h>
#include <src/microphone.h>
using namespace std::chrono_literals;

namespace {
  /** @brief Construct a deterministic audio frame. */
  microphone::frame_t frame(uint32_t seq, const char *audio = "opus") {
    return {true, seq, uint64_t(seq) * 960, audio};
  }
}  // namespace

TEST(Microphone, FramingRejectsTruncatedOversizedUnknownAndInconsistentPackets) {
  EXPECT_FALSE(microphone::parse(""));
  std::string packet(16, '\0');
  packet[0] = 1;
  packet[2] = char(0xc0);
  packet[3] = 3;
  auto stop = microphone::parse(packet);
  ASSERT_TRUE(stop);
  EXPECT_FALSE(stop->active);
  packet[1] = 1;
  EXPECT_FALSE(microphone::parse(packet));
  packet += "abc";
  packet[4] = char(0xff);
  packet[7] = char(0x80);
  packet[8] = 1;
  packet[15] = 1;
  auto audio = microphone::parse(packet);
  ASSERT_TRUE(audio);
  EXPECT_TRUE(audio->active);
  EXPECT_EQ(audio->sequence, 0x800000ffu);
  EXPECT_EQ(audio->timestamp, 0x0100000000000001ull);
  EXPECT_EQ(audio->opus, "abc");
  packet[0] = 2;
  EXPECT_FALSE(microphone::parse(packet));
  packet[0] = 1;
  packet[1] = 2;
  EXPECT_FALSE(microphone::parse(packet));
  packet[1] = 1;
  packet[2] = 0;
  EXPECT_FALSE(microphone::parse(packet));
  packet[2] = char(0xc0);
  packet.resize(1041);
  EXPECT_FALSE(microphone::parse(packet));
}

TEST(Microphone, ReordersPrimesAndRejectsReplays) {
  microphone::jitter_t q;
  auto t = microphone::clock::time_point {};
  EXPECT_TRUE(q.push(frame(0, "zero"), t));
  EXPECT_TRUE(q.push(frame(2, "two"), t + 20ms));
  EXPECT_TRUE(q.push(frame(1, "one"), t + 30ms));
  EXPECT_FALSE(q.push(frame(1), t + 31ms));
  EXPECT_FALSE(q.pop(t + 39ms));
  ASSERT_TRUE(q.pop(t + 40ms));  // zero
  EXPECT_EQ(q.pop(t + 60ms), "one");
  EXPECT_EQ(q.pop(t + 80ms), "two");
  EXPECT_FALSE(q.push(frame(0), t + 81ms));
}

TEST(Microphone, GrowsToEightyMsThenUsesLossConcealment) {
  microphone::jitter_t q;
  auto t = microphone::clock::time_point {};
  q.push(frame(0), t);
  EXPECT_EQ(q.pop(t + 40ms), "opus");
  EXPECT_FALSE(q.pop(t + 60ms));
  EXPECT_EQ(q.target_ms(), 60);
  EXPECT_FALSE(q.pop(t + 80ms));
  EXPECT_EQ(q.target_ms(), 80);
  auto plc = q.pop(t + 100ms);
  ASSERT_TRUE(plc);
  EXPECT_TRUE(plc->empty());
  EXPECT_FALSE(q.live(t + 501ms));
}

TEST(Microphone, StopCannotBeReorderedBehindAudioAndResumeCannotReplay) {
  microphone::jitter_t q;
  auto t = microphone::clock::time_point {};
  q.push(frame(10), t);
  EXPECT_TRUE(q.push({false, 11, 0, {}}, t + 10ms));
  EXPECT_FALSE(q.live(t + 10ms));
  EXPECT_FALSE(q.push(frame(10), t + 20ms));
  EXPECT_TRUE(q.push(frame(12), t + 30ms));
  EXPECT_TRUE(q.live(t + 30ms));
  EXPECT_FALSE(q.push({false, 11, 0, {}}, t + 40ms));
  EXPECT_TRUE(q.live(t + 40ms));
}

TEST(Microphone, StopBeforeFirstAudioRetainsReplayBoundaryAcrossWrap) {
  microphone::jitter_t q;
  auto t = microphone::clock::time_point {};
  EXPECT_TRUE(q.push({false, 0xffffffffu, 0, {}}, t));
  EXPECT_FALSE(q.push(frame(0xfffffffeu), t + 10ms));
  EXPECT_FALSE(q.push(frame(0xffffffffu), t + 20ms));
  EXPECT_FALSE(q.live(t + 20ms));
  EXPECT_TRUE(q.push({true, 0, 0, "resumed"}, t + 30ms));
  EXPECT_TRUE(q.live(t + 30ms));
  EXPECT_EQ(q.pop(t + 30ms + std::chrono::milliseconds(q.target_ms())), "resumed");
  EXPECT_FALSE(q.push({false, 0xfffffffeu, 0, {}}, t + 120ms));
  EXPECT_TRUE(q.live(t + 120ms));
}

TEST(Microphone, SequenceWrapAndBoundedBurstRecovery) {
  microphone::jitter_t q;
  auto t = microphone::clock::time_point {};
  EXPECT_TRUE(q.push({true, 0xffffffffu, 0, "last"}, t));
  EXPECT_TRUE(q.push({true, 0, 960, "first"}, t + 20ms));
  EXPECT_EQ(q.pop(t + 40ms), "last");
  EXPECT_EQ(q.pop(t + 60ms), "first");
  for (uint32_t i = 1; i < 1000; ++i) {
    q.push(frame(i), t + 80ms);
  }
  auto recovered = q.pop(t + 80ms);
  ASSERT_TRUE(recovered);
  EXPECT_EQ(*recovered, "opus");
}

TEST(Microphone, PcmClearsSpeechAndZeroFillsGraphUnderruns) {
  microphone::pcm_queue_t q;
  std::array<int16_t, microphone::frame_samples> frame;
  frame.fill(123);
  q.write(frame.data());
  std::array<int16_t, 1200> output;
  q.read(output.data(), output.size());
  for (size_t i = 0; i < output.size(); ++i) {
    EXPECT_EQ(output[i], i < frame.size() ? 123 : 0);
  }
  q.write(frame.data());
  q.clear();
  q.read(output.data(), output.size());
  for (auto sample : output) {
    EXPECT_EQ(sample, 0);
  }
}

TEST(Microphone, PcmOverrunLeavesHeadroomForVariableGraphQuanta) {
  microphone::pcm_queue_t q;
  std::array<int16_t, microphone::frame_samples> input;
  int16_t sample = 1;
  auto write = [&] {
    for (auto &value : input) {
      value = sample++;
    }
    q.write(input.data());
  };
  // A recorder attaches after the source has accumulated the full 40 ms bound.
  write();
  write();
  std::array<int16_t, 256> output;
  int previous = 0;
  int discontinuities = 0;
  for (int time = 0; time < 24000; ++time) {
    if (time % 960 == 1) {
      write();
    }
    if (time % 256 == 0) {
      q.read(output.data(), output.size());
      for (auto value : output) {
        ASSERT_GT(value, 0);
        if (previous && value != previous + 1) {
          ++discontinuities;
          EXPECT_EQ(value - previous, microphone::frame_samples + 1);
        }
        previous = value;
      }
    }
  }
  // Evict one stale frame once; partial trims would keep clipping later quanta.
  EXPECT_EQ(discontinuities, 1);
}
