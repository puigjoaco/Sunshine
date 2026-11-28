/**
 * @file src/stream_resize.h
 * @brief Porta v1 wire messages and session-local resize transaction state.
 */
#pragma once

#include <array>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string_view>
#include <utility>

namespace stream_resize {
  constexpr uint16_t request_type = 0x5507;  ///< Encrypted client request.
  constexpr uint16_t result_type = 0x5508;  ///< Reliable encrypted host result.

  /** @brief One fixed-size little-endian resize request. */
  struct request_t {
    uint32_t sequence;  ///< Strictly increasing nonzero session sequence.
    uint16_t width;  ///< Physical encoder width.
    uint16_t height;  ///< Physical encoder height.
  };

  /** @brief Read a bounded little-endian integer from a validated message. */
  inline uint32_t read_le(std::string_view bytes, size_t offset, size_t count) {
    uint32_t value = 0;
    for (size_t i = 0; i < count; ++i) {
      value |= uint32_t(uint8_t(bytes[offset + i])) << (8 * i);
    }
    return value;
  }

  /** @brief Parse exactly the eight-byte v1 payload, without packed/unaligned reads. */
  inline std::optional<request_t> parse(std::string_view payload) {
    if (payload.size() != 8) {
      return std::nullopt;
    }
    return request_t {read_le(payload, 0, 4), uint16_t(read_le(payload, 4, 2)), uint16_t(read_le(payload, 6, 2))};
  }

  /** @brief Validate the v1 even dimension bounds; encoder creation validates hardware limits. */
  inline bool valid(request_t request) {
    return request.sequence && request.width >= 256 && request.height >= 256 && request.width <= 4096 && request.height <= 4096 && !(request.width & 1) && !(request.height & 1);
  }

  /** @brief A result only becomes successful after encoder initialization. */
  struct result_t {
    request_t mode;  ///< Answered sequence and current stream dimensions.
    bool failed;  ///< Rejected request or failed transaction.

    /** @brief Serialize the ten-byte v1 result payload, including its zero reserved byte. */
    std::array<char, 10> payload() const {
      std::array<char, 10> bytes {};
      for (int i = 0; i < 4; ++i) {
        bytes[i] = char(mode.sequence >> (8 * i));
      }
      bytes[4] = failed;
      bytes[6] = char(mode.width);
      bytes[7] = char(mode.width >> 8);
      bytes[8] = char(mode.height);
      bytes[9] = char(mode.height >> 8);
      return bytes;
    }
  };

  /**
   * @brief Small locked mailbox; no compositor, encoder or control I/O occurs under this lock.
   * The capture thread owns mode changes; the encoder confirms or requests rollback.
   */
  class state_t {
  public:
    /** @brief Initialize failure replies with the launch stream's dimensions. */
    state_t(int width, int height):
        current {0, uint16_t(width), uint16_t(height)} {}

    /** @brief Allow requests only for the actual sole owned virtual capture. */
    void enable(bool value) {
      std::lock_guard lock(mutex);
      enabled = value;
      if (!enabled && pending) {
        result = result_t {{pending->sequence, current.width, current.height}, true};
        pending.reset();
      }
    }

    /** @brief Drop replayed requests and coalesce pending work to the newest sequence. */
    void offer(request_t request) {
      std::lock_guard lock(mutex);
      if (!request.sequence || request.sequence <= latest) {
        return;
      }
      latest = request.sequence;
      pending.reset();
      if (!enabled || !valid(request)) {
        result = result_t {{request.sequence, current.width, current.height}, true};
      } else {
        pending = request;
      }
    }

    /** @brief Check whether capture should stop for new work or a rollback. */
    bool needs_capture() {
      std::lock_guard lock(mutex);
      return rollback || (!active && pending.has_value());
    }

    /** @brief Claim a transaction after all encoders release the old capture. */
    std::optional<request_t> begin() {
      std::lock_guard lock(mutex);
      if (rollback) {
        return request_t {active->sequence, current.width, current.height};
      }
      if (active || !pending) {
        return std::nullopt;
      }
      active = std::exchange(pending, std::nullopt);
      return active;
    }

    /** @brief Publish dimensions after capture negotiation; wait for an encoder confirmation. */
    void captured(request_t mode) {
      std::lock_guard lock(mutex);
      candidate = mode;
    }

    /** @brief Use candidate dimensions only after the new capture is ready. */
    request_t dimensions() {
      std::lock_guard lock(mutex);
      return candidate.value_or(current);
    }

    /** @brief Request the known working mode if capture or encoder construction fails. */
    void failed() {
      std::lock_guard lock(mutex);
      if (active) {
        rollback = true;
        candidate.reset();
      }
    }

    /** @brief Confirm a fully initialized encoder and publish one result. */
    bool encoded() {
      std::lock_guard lock(mutex);
      if (!candidate || !active) {
        return false;
      }
      current = *candidate;
      if (!result || result->mode.sequence <= current.sequence) {
        result = result_t {current, rollback};
      }
      active.reset();
      candidate.reset();
      rollback = false;
      return true;
    }

    /** @brief Drain a result on the control thread, which alone owns ENet and control encryption. */
    std::optional<result_t> take_result() {
      std::lock_guard lock(mutex);
      auto answer = std::exchange(result, std::nullopt);
      if (answer) {
        answer->mode.width = current.width;
        answer->mode.height = current.height;
      }
      return answer;
    }

  private:
    std::mutex mutex;  ///< Protects only the small mailbox below.
    request_t current;  ///< Last encoder-confirmed stream dimensions.
    uint32_t latest = 0;  ///< Replay boundary, without sequence wraparound.
    bool enabled = false;  ///< Actual capture ownership, never inferred from a request.
    bool rollback = false;  ///< Old dimensions must be restored before replying failure.
    std::optional<request_t> pending;  ///< Newest request not yet claimed by capture.
    std::optional<request_t> active;  ///< Transaction awaiting capture/encoder completion.
    std::optional<request_t> candidate;  ///< Negotiated capture awaiting encoder confirmation.
    std::optional<result_t> result;  ///< Latest completed or rejected request.
  };
}  // namespace stream_resize
