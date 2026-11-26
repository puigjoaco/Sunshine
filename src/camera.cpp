/**
 * @file src/camera.cpp
 * @brief Decode bounded phone H.264 frames with system ffmpeg into v4l2loopback.
 */
#include "camera.h"

#include "logging.h"
#ifdef __linux__
  #include <atomic>
  #include <condition_variable>
  #include <fcntl.h>
  #include <filesystem>
  #include <fstream>
  #include <mutex>
  #include <poll.h>
  #include <signal.h>
  #include <spawn.h>
  #include <sys/wait.h>
  #include <thread>
  #include <unistd.h>
extern char **environ;  ///< Child inherits only the host environment.

namespace camera {
  namespace {
    std::atomic_bool claimed = false;  ///< One session owns the fixed camera device.

    /** @brief Worker owns decoder, all pipes and all device I/O. */
    class receiver_impl final: public receiver_t {
    public:
      receiver_impl():
          worker([this] {
            run();
          }) {}  ///< Start asynchronous discovery.

      /** @brief Join the worker after it blacks, closes and reaps its children. */
      ~receiver_impl() override {
        {
          std::lock_guard lock(mutex);
          quitting = true;
        }
        wake.notify_one();
        worker.join();
        claimed = false;
      }

      /**
       * @brief Reassemble bounded chunks and queue complete access units.
       * @param c Validated data chunk or sequenced stop.
       */
      void push(chunk_t c) override {
        std::lock_guard lock(mutex);
        if (disabled) {
          return;
        }
        if (!c.stop) {
          last_packet = microphone::clock::now();
        }
        auto f = assembler.push(std::move(c));
        if (!f) {
          return;
        }
        // Clearing queued dependent frames requires an IDR, never resume at a P frame.
        if (f->stop || frames.size() >= 3) {
          frames.clear();
          if (f->stop || !f->keyframe) {
            assembler.invalidate();
          }
          ++generation;
          if (!f->stop && !f->keyframe) {
            return;
          }
        }
        frames.push_back(std::move(*f));
        wake.notify_one();
      }

    private:
      /** @brief Reap with a bounded grace period, killing a stalled task-owned child. */
      void reap() {
        if (input >= 0) {
          close(input);
          input = -1;
        }
        if (child <= 0) {
          return;
        }
        auto until = microphone::clock::now() + std::chrono::milliseconds(400);
        while (true) {
          auto result = waitpid(child, nullptr, WNOHANG);
          if (result == child || (result < 0 && errno == ECHILD)) {
            break;
          }
          if (result < 0 && errno == EINTR) {
            continue;
          }
          if (result < 0 || microphone::clock::now() >= until) {
            kill(child, SIGKILL);
            while (waitpid(child, nullptr, 0) < 0 && errno == EINTR) {}
            break;
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        BOOST_LOG(debug) << "Phone camera reaped ffmpeg pid=" << child;
        child = -1;
      }

      /**
       * @brief Spawn ffmpeg with no shell and no inherited camera input pipe ends.
       * @param args Complete argument vector including program name.
       * @param pipe_input Whether to create a nonblocking compressed-input pipe.
       * @return True if the child was started.
       */
      bool spawn(std::vector<std::string> args, bool pipe_input) {
        int fds[2] {-1, -1};
        if (pipe_input && pipe2(fds, O_CLOEXEC) != 0) {
          return false;
        }
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        if (pipe_input) {
          posix_spawn_file_actions_adddup2(&actions, fds[0], STDIN_FILENO);
          posix_spawn_file_actions_addclose(&actions, fds[0]);
          posix_spawn_file_actions_addclose(&actions, fds[1]);
        } else {
          posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
        }
        posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
        posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
        std::vector<char *> argv;
        for (auto &a : args) {
          argv.push_back(a.data());
        }
        argv.push_back(nullptr);
        auto error = posix_spawn(&child, "/usr/bin/ffmpeg", &actions, nullptr, argv.data(), environ);
        posix_spawn_file_actions_destroy(&actions);
        if (fds[0] >= 0) {
          close(fds[0]);
        }
        if (error) {
          if (fds[1] >= 0) {
            close(fds[1]);
          }
          child = -1;
          return false;
        }
        std::error_code path_error;
        BOOST_LOG(debug) << "Phone camera ffmpeg pid=" << child << " cwd=" << std::filesystem::current_path(path_error).string();
        if (pipe_input) {
          input = fds[1];
          fcntl(input, F_SETFL, fcntl(input, F_GETFL) | O_NONBLOCK);
        }
        return true;
      }

      /**
       * @brief Restart the external decoder only at a complete IDR.
       * @param degrees Clockwise rotation to upright.
       * @return True if the decoder process started.
       */
      bool start(uint16_t degrees) {
        reap();
        rotation = degrees;
        std::string filter;
        if (degrees == 90) {
          filter = "transpose=clock,";
        } else if (degrees == 180) {
          filter = "hflip,vflip,";
        } else if (degrees == 270) {
          filter = "transpose=cclock,";
        }
        filter += "scale=1280:720:force_original_aspect_ratio=increase,crop=1280:720,setsar=1";
        return spawn({"/usr/bin/ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "error", "-fflags", "+discardcorrupt", "-flags", "low_delay", "-threads", "1", "-probesize", "32", "-analyzeduration", "0", "-f", "h264", "-r", "30", "-i", "pipe:0", "-vf", filter, "-threads", "1", "-pix_fmt", "yuv420p", "-c:v", "rawvideo", "-fps_mode", "passthrough", "-f", "v4l2", device}, true);
      }

      /** @brief Send black on explicit off, input idle and teardown, then release the device. */
      void stop() {
        bool was_active = shown;
        shown = false;
        reap();
        if (was_active && spawn({"/usr/bin/ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i", "color=c=black:s=1280x720:r=30", "-frames:v", "3", "-threads", "1", "-pix_fmt", "yuv420p", "-c:v", "rawvideo", "-f", "v4l2", device}, false)) {
          reap();
        }
      }

      /**
       * @brief Bound time spent writing a compressed frame to a failed decoder.
       * @param data Complete Annex-B access unit.
       * @return True if every byte was written within the deadline.
       */
      bool write(const std::string &data) {
        auto until = microphone::clock::now() + std::chrono::milliseconds(100);
        size_t offset = 0;
        while (offset < data.size()) {
          if (microphone::clock::now() >= until) {
            return false;
          }
          pollfd fd {input, POLLOUT, 0};
          auto ready = poll(&fd, 1, 10);
          if (ready < 0 && errno == EINTR) {
            continue;
          }
          if (ready < 0 || (fd.revents & (POLLERR | POLLHUP | POLLNVAL))) {
            return false;
          }
          if (!ready) {
            continue;
          }
          auto n = ::write(input, data.data() + offset, data.size() - offset);
          if (n > 0) {
            offset += n;
          } else if (n < 0 && errno != EAGAIN && errno != EINTR) {
            return false;
          }
        }
        return true;
      }

      /** @brief Discover dependencies once and run bounded frame processing until session teardown. */
      void run() {
        // A dead ffmpeg must never deliver SIGPIPE to Sunshine.
        sigset_t signals;
        sigemptyset(&signals);
        sigaddset(&signals, SIGPIPE);
        pthread_sigmask(SIG_BLOCK, &signals, nullptr);
        std::error_code ec;
        auto entry = std::filesystem::directory_iterator("/sys/class/video4linux", ec);
        for (; !ec && entry != std::filesystem::directory_iterator(); entry.increment(ec)) {
          std::ifstream name(entry->path() / "name");
          std::string label;
          std::getline(name, label);
          if (label == "Sunshine Camera") {
            device = "/dev/" + entry->path().filename().string();
            break;
          }
        }
        if (device.empty() || access(device.c_str(), R_OK | W_OK) || access("/usr/bin/ffmpeg", X_OK)) {
          BOOST_LOG(warning) << "Phone camera disabled: /usr/bin/ffmpeg or writable Sunshine Camera device unavailable";
          std::lock_guard lock(mutex);
          disabled = true;
          frames.clear();
          return;
        }
        uint64_t seen_generation = 0;
        bool need_idr = true;
        while (true) {
          std::optional<frame_t> f;
          bool idle = false;
          {
            std::unique_lock lock(mutex);
            wake.wait_for(lock, std::chrono::milliseconds(50), [&] {
              return quitting || !frames.empty();
            });
            if (quitting) {
              break;
            }
            if (generation != seen_generation) {
              need_idr = true;
              seen_generation = generation;
            }
            idle = microphone::clock::now() - last_packet > std::chrono::seconds(1);
            if (!frames.empty()) {
              f = std::move(frames.front());
              frames.pop_front();
            }
          }
          if (!f) {
            if (shown && idle) {
              stop();
              need_idr = true;
              std::lock_guard lock(mutex);
              assembler.invalidate();
            }
            continue;
          }
          if (f->stop) {
            stop();
            need_idr = true;
            continue;
          }
          if ((need_idr || child <= 0 || rotation != f->rotation) && !f->keyframe) {
            continue;
          }
          if (child <= 0 || rotation != f->rotation) {
            if (!start(f->rotation)) {
              need_idr = true;
              continue;
            }
            shown = true;
          }
          if (!write(f->data)) {
            reap();
            need_idr = true;
            std::lock_guard lock(mutex);
            frames.clear();
            assembler.invalidate();
          } else {
            need_idr = false;
          }
        }
        stop();
      }

      std::mutex mutex;  ///< Protect reassembly and the bounded output queue.
      std::condition_variable wake;  ///< Worker notification.
      assembler_t assembler;  ///< At most one 384 KiB frame.
      std::deque<frame_t> frames;  ///< At most three complete frames.
      bool quitting = false, disabled = false;  ///< Lifecycle flags under mutex.
      microphone::clock::time_point last_packet {};  ///< Last input activity, including P frames suppressed after loss.
      uint64_t generation = 0;  ///< Queue loss forces decoder recovery.
      std::string device;  ///< Discovered by the exact sysfs card label.
      int input = -1;  ///< Nonblocking child input, worker only.
      pid_t child = -1;  ///< Owned decoder/black-generator process, worker only.
      uint16_t rotation = 0;  ///< Current decoder rotation.
      bool shown = false;  ///< The device may still show a phone image that stop() must black out, worker only.
      std::thread worker;  ///< Joined before session resources disappear.
    };
  }  // namespace

  bool available() {
    return true;
  }

  std::unique_ptr<receiver_t> create() {
    if (claimed.exchange(true)) {
      return nullptr;
    }
    try {
      return std::make_unique<receiver_impl>();
    } catch (...) {
      claimed = false;
      throw;
    }
  }
}  // namespace camera
#else
namespace camera {
  bool available() {
    return false;
  }

  std::unique_ptr<receiver_t> create() {
    return nullptr;
  }
}  // namespace camera
#endif
