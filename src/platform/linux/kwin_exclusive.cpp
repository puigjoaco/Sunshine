/**
 * @file src/platform/linux/kwin_exclusive.cpp
 * @brief Exclusive KWin output lease and crash-guardian communication.
 */
#include "kwin_exclusive.h"

#include "src/input.h"
#include "src/logging.h"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <nlohmann/json.hpp>
#include <poll.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

/** @brief Process environment inherited by the independent recovery guardian. */
extern char **environ;

using namespace std::chrono_literals;

namespace kwin {
  exclusive_t &exclusive_t::instance() {
    static exclusive_t lease;
    return lease;
  }

  exclusive_t::exclusive_t():
      worker([this] {
        run();
      }) {}

  exclusive_t::~exclusive_t() {
    {
      std::lock_guard lock(mutex);
      stopping = true;
      changed.notify_all();
    }
    worker.join();
    disconnect();
  }

  bool exclusive_t::request(const std::string &command) {
    if (channel < 0) {
      int sockets[2];
      if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) < 0) {
        return false;
      }
      posix_spawn_file_actions_t actions;
      posix_spawn_file_actions_init(&actions);
      posix_spawn_file_actions_adddup2(&actions, sockets[1], STDIN_FILENO);
      posix_spawn_file_actions_adddup2(&actions, sockets[1], STDOUT_FILENO);
      // Do not inherit Sunshine sockets (including a client or Wayland socket).
      posix_spawn_file_actions_addclosefrom_np(&actions, STDERR_FILENO + 1);
      char python[] = "/usr/bin/python3";
      char script[] = SUNSHINE_ASSETS_DIR "/kwin-exclusive.py";
      char watch[] = "watch";
      char *args[] = {python, script, watch, nullptr};
      pid_t pid;
      auto error = posix_spawn(&pid, python, &actions, nullptr, args, environ);
      posix_spawn_file_actions_destroy(&actions);
      close(sockets[1]);
      if (error) {
        close(sockets[0]);
        return false;
      }
      channel = sockets[0];
      guardian = pid;
    }
    const auto end = std::chrono::steady_clock::now() + 40s;
    auto message = command + "\n";
    size_t offset = 0;
    while (offset < message.size()) {
      auto sent = send(channel, message.data() + offset, message.size() - offset, MSG_NOSIGNAL | MSG_DONTWAIT);
      if (sent > 0) {
        offset += sent;
      } else if (sent < 0 && (errno == EAGAIN || errno == EINTR) && std::chrono::steady_clock::now() < end) {
        pollfd fd {channel, POLLOUT, 0};
        poll(&fd, 1, 100);
      } else {
        return false;
      }
    }
    std::string reply;
    while (std::chrono::steady_clock::now() < end) {
      pollfd fd {channel, POLLIN, 0};
      if (poll(&fd, 1, 100) <= 0) {
        continue;
      }
      char byte;
      if (recv(channel, &byte, 1, MSG_DONTWAIT) != 1) {
        return false;
      }
      if (byte == '\n') {
        auto response = nlohmann::json::parse(reply, nullptr, false);
        cursor.reset();
        if (response.is_object() && response.contains("pointer") && response["pointer"].is_object()) {
          const auto &p = response["pointer"];
          if (p.contains("x") && p.contains("y") && p.contains("width") && p.contains("height") &&
              p["x"].is_number() && p["y"].is_number() && p["width"].is_number() && p["height"].is_number()) {
            cursor = std::array<float, 4> {p["x"].get<float>(), p["y"].get<float>(), p["width"].get<float>(), p["height"].get<float>()};
          }
        }
        return response.is_object() && response.value("ok", false);
      }
      reply += byte;
      if (reply.size() > 4096) {
        return false;
      }
    }
    return false;
  }

  void exclusive_t::disconnect() {
    if (channel >= 0) {
      close(channel);  // EOF asks the guardian to restore even after a timeout.
      channel = -1;
    }
    if (guardian > 0) {
      const auto end = std::chrono::steady_clock::now() + 40s;
      int status;
      while (waitpid(guardian, &status, WNOHANG) == 0 && std::chrono::steady_clock::now() < end) {
        std::this_thread::sleep_for(20ms);
      }
      // Never kill a helper that may still be recovering the desk.
      guardian = -1;
    }
  }

  bool exclusive_t::begin(const void *identity, const std::string &name) {
    std::lock_guard lock(mutex);
    if ((active && active != identity) || stopping) {
      return false;
    }
    if (!request(nlohmann::json {{"action", "enter"}, {"output", name}}.dump())) {
      BOOST_LOG(error) << "[kwingrab] Exclusive mode failed; restoring before capture fallback";
      disconnect();
      active = nullptr;
      parked.reset();
      changed.notify_all();
      return false;
    }
    // The retained or newly created output is now primary and owns the windows.
    active = identity;
    parked.reset();
    changed.notify_all();
    return true;
  }

  bool exclusive_t::prepare(const void *identity) {
    std::lock_guard lock(mutex);
    if (active || parked || stopping) {
      return false;
    }
    if (!request(R"({"action":"prepare"})")) {
      disconnect();
      return false;
    }
    active = identity;
    immediate_restore = false;
    return true;
  }

  std::shared_ptr<void> exclusive_t::resume() {
    std::lock_guard lock(mutex);
    if (active || stopping || !parked) {
      return nullptr;
    }
    immediate_restore = false;
    active = parked.get();
    auto output = std::move(parked);
    changed.notify_all();
    return output;
  }

  void exclusive_t::finish(std::shared_ptr<void> output, int delay) {
    std::lock_guard lock(mutex);
    if (output.get() != active) {
      return;
    }
    active = nullptr;
    parked = std::move(output);
    const bool synchronous = delay == 0 && !immediate_restore;
    delay = immediate_restore ? 0 : delay;
    BOOST_LOG(info) << "[kwingrab] Exclusive capture teardown; reconnect grace=" << delay << "s";
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(delay);
    // A normal disconnect wakes the joined lease worker immediately. Recovery
    // must not block the capture destructor under the session's hang watchdog.
    if (synchronous) {
      restore();
    }
    changed.notify_all();
  }

  void exclusive_t::session_stopping(bool rotation) {
    std::lock_guard lock(mutex);
    immediate_restore = !rotation;
    if (parked && immediate_restore) {
      deadline = std::chrono::steady_clock::now();
      changed.notify_all();
    }
  }

  void exclusive_t::restore() {
    const auto started = std::chrono::steady_clock::now();
    // Remove the virtual wl_output after physical scanouts return, before final
    // window configures. Chromium may otherwise retain its phone work-area bound.
    const bool outputs_restored = request(R"({"action":"restore-outputs"})");
    parked.reset();
    if (!outputs_restored || !request(R"({"action":"restore"})")) {
      BOOST_LOG(error) << "[kwingrab] Exclusive restore incomplete; guardian/saved snapshot retained for emergency recovery";
      disconnect();
    }
    parked.reset();
    if (cursor) {
      const auto [x, y, width, height] = *cursor;
      input::position_cursor({0, 0, static_cast<int>(width), static_cast<int>(height), static_cast<int>(width), static_cast<int>(height)}, x, y);
      cursor.reset();
    }
    BOOST_LOG(info) << "[kwingrab] Exclusive desktop recovery finished in "
                    << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count() << "ms";
  }

  void exclusive_t::run() {
    std::unique_lock lock(mutex);
    while (!stopping) {
      if (!parked) {
        changed.wait_for(lock, 250ms);
      } else {
        changed.wait_until(lock, std::min(deadline, std::chrono::steady_clock::now() + 250ms));
      }
      if (parked && std::chrono::steady_clock::now() >= deadline) {
        restore();
      }
      if ((active || parked) && channel >= 0) {
        pollfd fd {channel, POLLIN, 0};
        if (poll(&fd, 1, 0) > 0 && (fd.revents & (POLLHUP | POLLERR))) {
          BOOST_LOG(error) << "[kwingrab] Exclusive guardian exited; recovering the saved desk immediately";
          disconnect();
          restore();
        }
      }
    }
    if (parked || active) {
      restore();
    }
  }
}  // namespace kwin
