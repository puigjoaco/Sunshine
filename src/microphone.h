/**
 * @file src/microphone.h
 * @brief Versioned phone microphone packets and bounded adaptive playout queue.
 */
#pragma once
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace microphone {
  constexpr uint16_t packet_type = 0x5505;  ///< Encrypted ENet microphone extension.
  constexpr int frame_samples = 960;  ///< One mono 20 ms frame at 48 kHz.
  using clock = std::chrono::steady_clock;  ///< Receiver clock, independent of phone clock.

  /** @brief Validated wire frame (all integer fields on the wire are little endian). */
  struct frame_t {
    bool active;  ///< False means release the microphone.
    uint32_t sequence;  ///< Monotonic modulo-2^32 packet counter.
    uint64_t timestamp;  ///< Capture timestamp in 48 kHz samples, no clock synchronization needed.
    std::string opus;  ///< One bounded Opus packet, empty for stop.
  };

  /**
   * @brief Read a bounded integer without alignment assumptions.
   * @param data Buffer containing at least offset + bytes bytes.
   * @param offset First byte of the integer in the buffer.
   * @param bytes Number of bytes to read, at most eight.
   * @return The unsigned little-endian integer.
   */
  inline uint64_t read_le(std::string_view data, size_t offset, size_t bytes) {
    uint64_t value = 0;
    for (size_t i = 0; i < bytes; ++i) {
      value |= uint64_t(uint8_t(data[offset + i])) << (i * 8);
    }
    return value;
  }

  /**
   * @brief Validate the complete v1 envelope before allocating or decoding audio.
   * @param data Complete microphone packet payload.
   * @return Validated frame, or no value if the envelope is invalid.
   */
  inline std::optional<frame_t> parse(std::string_view data) {
    if (data.size() < 16 || data.size() > 1040 || data[0] != 1 ||
        (data[1] != 0 && data[1] != 1) || read_le(data, 2, 2) != frame_samples ||
        (data[1] == 0 ? data.size() != 16 : data.size() == 16)) {
      return std::nullopt;
    }
    return frame_t {data[1] == 1, uint32_t(read_le(data, 4, 4)), read_le(data, 8, 8), std::string(data.substr(16))};
  }

  /**
   * @brief Bounded reorder queue. Caller serializes access, playout runs every 20 ms.
   *
   * Starts at 40 ms, grows toward 80 ms on late arrivals, and shrinks after stable
   * delivery. Sender timestamps detect stale bursts without comparing wall clocks.
   */
  class jitter_t {
  public:
    /**
     * @brief Queue a frame or accept a sequenced stop.
     * @param frame Validated audio or stop frame.
     * @param now Monotonic arrival time.
     * @return True if accepted, false for duplicate or stale input.
     */
    bool push(frame_t frame, clock::time_point now) {
      if (have_sequence && int32_t(frame.sequence - newest) <= 0) {
        if (!frame.active || !active || int32_t(frame.sequence - expected) < 0) {
          target = std::min(4, target + 1);
          return false;
        }
      } else {
        newest = frame.sequence;
        have_sequence = true;
      }
      if (!frame.active) {
        clear();
        return true;
      }
      if (!active) {
        active = true;
        expected = frame.sequence;
        next_timestamp = frame.timestamp;
        start = now + std::chrono::milliseconds(20 * target);
      }
      if (started && int32_t(frame.sequence - expected) < 0) {
        return false;
      }
      for (const auto &f : frames) {
        if (f.sequence == frame.sequence) {
          return false;
        }
      }
      // Re-anchor after a stall or a sender restart; never replay accumulated old audio.
      if (int32_t(frame.sequence - expected) > 8 || frame.timestamp > next_timestamp + 8 * frame_samples) {
        frames.clear();
        expected = frame.sequence;
        next_timestamp = frame.timestamp;
        started = false;
        start = now + std::chrono::milliseconds(20 * target);
      }
      last = now;
      auto pos = frames.begin();
      while (pos != frames.end() && int32_t(pos->sequence - frame.sequence) < 0) {
        ++pos;
      }
      frames.insert(pos, std::move(frame));
      while (frames.size() > 8) {
        frames.pop_front();
        expected = frames.front().sequence;
        next_timestamp = frames.front().timestamp;
      }
      return true;
    }

    /**
     * @brief Produce the next frame at a playout tick.
     * @param now Monotonic playout time.
     * @return Opus payload, an empty payload for PLC, or no value while priming or idle.
     */
    std::optional<std::string> pop(clock::time_point now) {
      if (!active || now < start) {
        return std::nullopt;
      }
      started = true;
      while (!frames.empty() && int32_t(frames.front().sequence - expected) < 0) {
        frames.pop_front();
      }
      std::string output;
      if (!frames.empty() && frames.front().sequence == expected) {
        output = std::move(frames.front().opus);
        next_timestamp = frames.front().timestamp;
        frames.pop_front();
        if (++stable >= 250) {
          if (target > 2) {
            --target;
            ++expected;
            next_timestamp += frame_samples;
          }
          stable = 0;
        }
      } else {
        stable = 0;
        if (target < 4) {
          ++target;
          return std::nullopt;  // Add 20 ms of real playout delay, then retry this frame.
        }
      }
      ++expected;
      next_timestamp += frame_samples;
      // Trim excessive queue delay rather than making a dictation app hear old speech.
      if (frames.size() > size_t(target + 1)) {
        expected = frames[frames.size() - target].sequence;
        next_timestamp = frames[frames.size() - target].timestamp;
      }
      return output;
    }

    /** @brief Clear audio after mute, idle timeout, or disconnect, retaining anti-replay sequence. */
    void clear() {
      frames.clear();
      active = started = false;
      stable = 0;
    }

    /**
     * @brief Check activity; timeout also covers lost stop packets and crashes.
     * @param now Monotonic time to compare with the most recent arrival.
     * @return True while active and within the 500 ms input timeout.
     */
    bool live(clock::time_point now) const {
      return active && now - last < std::chrono::milliseconds(500);
    }

    /**
     * @brief Get the current adaptive target.
     * @return Target playout delay in milliseconds.
     */
    int target_ms() const {
      return target * 20;
    }

  private:
    std::deque<frame_t> frames;  ///< Bounded, ordered Opus frames.
    uint32_t newest = 0;  ///< Newest accepted sequence, retained as the replay boundary.
    uint32_t expected = 0;  ///< Next playout sequence.
    uint64_t next_timestamp = 0;  ///< Next sender capture timestamp.
    bool have_sequence = false;  ///< Whether a replay boundary has been established.
    bool active = false;  ///< Whether audio has arrived since the last stop or clear.
    bool started = false;  ///< Whether playout has begun after priming.
    int target = 2;  ///< Adaptive target in frames.
    int stable = 0;  ///< Consecutive healthy outputs.
    clock::time_point start {};  ///< Prime deadline.
    clock::time_point last {};  ///< Most recent accepted audio packet arrival.
  };

  /** @brief Session-owned receiver; no platform audio work runs on the control thread. */
  class receiver_t {
  public:
    /** @brief Release session-owned receiver resources. */
    virtual ~receiver_t() = default;
    /**
     * @brief Submit a validated packet; implementations bound queue memory.
     * @param frame Audio or stop frame to enqueue.
     */
    virtual void push(frame_t frame) = 0;
  };

  /**
   * @brief Check virtual microphone support in this build.
   * @return True if the platform audio dependencies were enabled at build time.
   */
  bool available();
  /**
   * @brief Claim the single virtual microphone for a session.
   * @return Session receiver, or null if unsupported or already owned by another session.
   */
  std::unique_ptr<receiver_t> create();
  /**
   * @brief Run the independent default-source guardian.
   * @param fd Inherited pipe descriptor whose EOF signals the end of the parent lifetime.
   * @return Zero after monitoring completes, or one if initialization fails or is unsupported.
   */
  int guard(int fd);
}  // namespace microphone
