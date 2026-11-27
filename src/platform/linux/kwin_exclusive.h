/**
 * @file src/platform/linux/kwin_exclusive.h
 * @brief Retain the exclusive output across reconnects and supervise recovery.
 */
#pragma once

#include <array>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace kwin {
  /**
   * @brief Process-wide single-client lease with an independent crash guardian.
   */
  class exclusive_t {
  public:
    /**
     * @brief Get the lease whose destructor restores immediately on normal exit.
     * @return Process-wide exclusive-output lease.
     */
    static exclusive_t &instance();
    /** @brief Restore and join the grace worker before disconnecting the guardian. */
    ~exclusive_t();
    /**
     * @brief Persist the original desktop before creating a virtual output.
     * @param identity Address of the new capture's screencast object.
     * @return True after a durable output and window snapshot.
     */
    bool prepare(const void *identity);
    /**
     * @brief Activate a configured output, preserving a pending original snapshot.
     * @param identity Address of the capture's screencast object.
     * @param name Owned output name.
     * @return True after the guardian verifies the sole enabled output.
     */
    bool begin(const void *identity, const std::string &name);
    /**
     * @brief Claim the parked output before its deadline, cancelling the timer.
     * @return Retained screencast output, or null if no parked output can be claimed.
     */
    std::shared_ptr<void> resume();
    /**
     * @brief Transfer output ownership to the reconnect grace worker.
     * @param output Screencast to keep alive until replacement or restoration.
     * @param delay Seconds of grace; zero restores synchronously on startup failure.
     */
    void finish(std::shared_ptr<void> output, int delay);
    /**
     * @brief Choose immediate teardown unless the ending stream requested a rotation reconnect.
     * @param rotation True only for an authenticated, unexpired rotation hint.
     */
    void session_stopping(bool rotation);

  private:
    /** @brief Start the joined grace worker without changing any output. */
    exclusive_t();
    /** @brief Start the guardian if needed and exchange one bounded JSON request. */
    bool request(const std::string &command);
    /** @brief Close the lifeline and reap the guardian without killing recovery. */
    void disconnect();
    /** @brief Restore the saved desk before releasing a parked output. */
    void restore();
    /** @brief Process reconnect deadlines and recover a lost guardian. */
    void run();

    std::mutex mutex;  ///< Serializes capture handoffs and the grace deadline.
    std::condition_variable changed;  ///< Wakes the grace worker on reconnect or exit.
    std::shared_ptr<void> parked;  ///< Keeps the old virtual output alive while disconnected.
    const void *active = nullptr;  ///< Borrowed identity of the actual capture owner.
    std::chrono::steady_clock::time_point deadline;  ///< Pending restore deadline.
    bool immediate_restore = false;  ///< Ordinary session ends bypass capture-reinit grace.
    bool stopping = false;  ///< Requests immediate exit restoration.
    int channel = -1;  ///< Private guardian lifeline; EOF means immediate restore.
    int guardian = -1;  ///< Child PID for bounded reaping.
    std::optional<std::array<float, 4>> cursor;  ///< Physical recovery position and desktop bounds, if needed.
    std::thread worker;  ///< Grace-period timer; no detached Sunshine thread.
  };
}  // namespace kwin
