/**
 * @file src/camera.h
 * @brief Bounded phone camera v1 parsing and loss recovery.
 */
#pragma once
#include "microphone.h"

#include <vector>

namespace camera {
  constexpr uint16_t packet_type = 0x5506;  ///< Encrypted control extension.
  constexpr size_t chunk_bytes = 768;  ///< Fits one encrypted ENet packet at MTU 900.
  constexpr size_t max_chunks = 512;  ///< Bounds each compressed frame to 384 KiB.

  /** @brief Validated chunk or sequenced stop. */
  struct chunk_t {
    bool keyframe = false;  ///< Sender marks an IDR with SPS/PPS.
    bool stop = false;  ///< Release the camera.
    uint16_t rotation = 0;  ///< Clockwise rotation to upright, in degrees.
    uint32_t sequence = 0;  ///< Frame sequence modulo 2^32.
    uint16_t index = 0;  ///< Zero-based chunk index.
    uint16_t count = 0;  ///< Number of chunks in this frame.
    uint64_t timestamp = 0;  ///< Capture timestamp in microseconds, sender clock.
    std::string data;  ///< Annex-B bytes.
  };

  /** @brief Complete compressed frame or stop marker. */
  struct frame_t {
    bool keyframe;  ///< Decoder recovery point.
    bool stop;  ///< Release marker.
    uint16_t rotation;  ///< Clockwise degrees.
    uint32_t sequence;  ///< Frame sequence.
    std::string data;  ///< Complete Annex-B access unit.
  };

  /**
   * @brief Validate a complete v1 camera chunk before allocating frame storage.
   * @param p Complete wire payload.
   * @return Validated chunk or no value for malformed input.
   */
  inline std::optional<chunk_t> parse(std::string_view p) {
    if (p.size() < 24 || p.size() > 24 + chunk_bytes || p[0] != 1 ||
        (uint8_t(p[1]) & ~3) || microphone::read_le(p, 4, 4)) {
      return std::nullopt;
    }
    chunk_t c;
    c.keyframe = p[1] & 1;
    c.stop = p[1] & 2;
    c.rotation = microphone::read_le(p, 2, 2);
    c.sequence = microphone::read_le(p, 8, 4);
    c.index = microphone::read_le(p, 12, 2);
    c.count = microphone::read_le(p, 14, 2);
    c.timestamp = microphone::read_le(p, 16, 8);
    if (c.rotation != 0 && c.rotation != 90 && c.rotation != 180 && c.rotation != 270) {
      return std::nullopt;
    }
    if (c.stop ? (c.keyframe || c.index || c.count || p.size() != 24) :
                 (!c.count || c.count > max_chunks || c.index >= c.count || p.size() == 24 ||
                  (c.index + 1 != c.count && p.size() != 24 + chunk_bytes))) {
      return std::nullopt;
    }
    c.data = p.substr(24);
    return c;
  }

  /** @brief One bounded frame in flight. Loss freezes output until a complete IDR. */
  class assembler_t {
  public:
    /**
     * @brief Consume a validated chunk, ignoring replayed and duplicate input.
     * @param c Validated wire chunk.
     * @return Complete decodable frame or stop, otherwise no value.
     */
    std::optional<frame_t> push(chunk_t c) {
      if (have && int32_t(c.sequence - newest) < 0) {
        return std::nullopt;
      }
      if (!have || c.sequence != newest) {
        if (have && (!parts.empty() || c.sequence != newest + 1)) {
          waiting = true;
        }
        parts.clear();
        received = 0;
        newest = c.sequence;
        have = true;
        finished = false;
        meta = c;
        meta.data.clear();
      }
      if (finished) {
        return std::nullopt;
      }
      if (c.stop) {
        parts.clear();
        waiting = true;
        finished = true;
        return frame_t {false, true, 0, c.sequence, {}};
      }
      if (c.rotation != meta.rotation || c.count != meta.count || c.timestamp != meta.timestamp ||
          c.keyframe != meta.keyframe || meta.stop) {
        invalidate();
        finished = true;
        return std::nullopt;
      }
      if (waiting && !c.keyframe) {
        finished = true;
        return std::nullopt;
      }
      if (parts.empty()) {
        parts.resize(c.count);
      }
      if (!parts[c.index].empty()) {
        return std::nullopt;
      }
      parts[c.index] = std::move(c.data);
      if (++received != c.count) {
        return std::nullopt;
      }
      frame_t f {c.keyframe, false, c.rotation, c.sequence, {}};
      f.data.reserve((c.count - 1) * chunk_bytes + parts.back().size());
      for (const auto &p : parts) {
        f.data += p;
      }
      parts.clear();
      finished = true;
      waiting = false;
      return f;
    }

    /** @brief Forget partial compressed state after queue overflow or decoder failure, retaining replay boundary. */
    void invalidate() {
      parts.clear();
      received = 0;
      waiting = true;
    }

  private:
    std::vector<std::string> parts;  ///< At most max_chunks strings.
    chunk_t meta;  ///< In-flight frame metadata.
    uint32_t newest = 0;  ///< Retained replay boundary.
    size_t received = 0;  ///< Unique chunks received.
    bool have = false;  ///< Boundary initialized.
    bool finished = false;  ///< Current sequence completed or discarded.
    bool waiting = true;  ///< Require an IDR before output.
  };

  /** @brief Session-owned asynchronous camera receiver. */
  class receiver_t {
  public:
    virtual ~receiver_t() = default;  ///< Stop and reap every owned child.
    /**
     * @brief Submit a validated chunk without decode or device I/O on the caller.
     * @param chunk Camera data or stop.
     */
    virtual void push(chunk_t chunk) = 0;
  };

  /**
   * @brief Check platform support for the external camera receiver.
   * @return True on Linux builds.
   */
  bool available();
  /**
   * @brief Claim the single virtual webcam for one session.
   * @return Single-owner receiver, or null while another session owns it.
   */
  std::unique_ptr<receiver_t> create();
}  // namespace camera
