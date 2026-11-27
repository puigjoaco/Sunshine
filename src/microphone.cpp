/**
 * @file src/microphone.cpp
 * @brief Decode phone Opus into a native PipeWire source and restore the default on exit/crash.
 */
#include "microphone.h"

#include "logging.h"
#ifdef SUNSHINE_BUILD_MICROPHONE
  #include <array>
  #include <atomic>
  #include <cstring>
  #include <fcntl.h>
  #include <mutex>
  #include <opus/opus.h>
  #include <pipewire/pipewire.h>
  #include <poll.h>
  #include <pulse/pulseaudio.h>
  #include <spa/param/audio/format-utils.h>
  #include <spawn.h>
  #include <sys/wait.h>
  #include <thread>
  #include <unistd.h>
extern char **environ;  ///< Process environment inherited by the guardian.

namespace microphone {
  namespace {
    std::atomic_bool claimed = false;  ///< One stable source name, one session owner.
    constexpr auto source_name = "sunshine_microphone";  ///< Stable app-visible source name.

    /** @brief Bounded synchronous Pulse connection, used only in the independent guardian. */
    struct pulse_t {
      pa_mainloop *loop = pa_mainloop_new();  ///< Private Pulse loop.
      pa_context *context = loop ? pa_context_new(pa_mainloop_get_api(loop), "Sunshine Microphone guardian") : nullptr;  ///< Server connection.
      std::string current;  ///< Current default source.
      bool exists = false;  ///< Whether the Sunshine source is registered.
      bool success = false;  ///< Result of the last default update.

      /** @brief Release Pulse handles. */
      ~pulse_t() {
        if (context) {
          pa_context_disconnect(context);
          pa_context_unref(context);
        }
        if (loop) {
          pa_mainloop_free(loop);
        }
      }

      /**
       * @brief Drive one asynchronous operation with a two-second upper bound.
       * @param op Operation to consume and unreference, or null on creation failure.
       * @return True if the operation completed before the deadline.
       */
      bool wait(pa_operation *op) {
        if (!op) {
          return false;
        }
        auto end = clock::now() + std::chrono::seconds(2);
        while (pa_operation_get_state(op) == PA_OPERATION_RUNNING && clock::now() < end) {
          pa_mainloop_iterate(loop, 0, nullptr);
          usleep(2000);
        }
        bool done = pa_operation_get_state(op) == PA_OPERATION_DONE;
        if (!done) {
          pa_operation_cancel(op);
        }
        pa_operation_unref(op);
        return done;
      }

      /**
       * @brief Establish a bounded connection.
       * @return True if the Pulse context became ready within two seconds.
       */
      bool connect() {
        if (!context || pa_context_connect(context, nullptr, PA_CONTEXT_NOAUTOSPAWN, nullptr) < 0) {
          return false;
        }
        auto end = clock::now() + std::chrono::seconds(2);
        while (clock::now() < end) {
          pa_mainloop_iterate(loop, 0, nullptr);
          auto state = pa_context_get_state(context);
          if (state == PA_CONTEXT_READY) {
            return true;
          }
          if (!PA_CONTEXT_IS_GOOD(state)) {
            break;
          }
          usleep(2000);
        }
        return false;
      }

      /**
       * @brief Read the default and check whether the native source is visible through Pulse.
       * @return True if both operations completed and a default source was returned.
       */
      bool query() {
        current.clear();
        exists = false;
        auto a = wait(pa_context_get_server_info(context, [](pa_context *, const pa_server_info *i, void *p) {
          if (i && i->default_source_name) {
            static_cast<pulse_t *>(p)->current = i->default_source_name;
          }
        },
                                                 this));
        auto b = wait(pa_context_get_source_info_by_name(context, source_name, [](pa_context *, const pa_source_info *i, int, void *p) {
          if (i) {
            static_cast<pulse_t *>(p)->exists = true;
          }
        },
                                                         this));
        return a && b && !current.empty();
      }

      /**
       * @brief Set the default source, mirroring Sunshine's Pulse default-sink operation.
       * @param name Pulse source name to select.
       * @return True if the server accepted the update before the deadline.
       */
      bool set(const std::string &name) {
        success = false;
        return wait(pa_context_set_default_source(context, name.c_str(), [](pa_context *, int ok, void *p) {
                 static_cast<pulse_t *>(p)->success = ok;
               },
                                                  this)) &&
               success;
      }
    };

    /** @brief A virtual source, clocked by PipeWire; process callbacks only drain bounded PCM. */
    class source_t {
    public:
      /** @brief Start the output stream with fixed 48 kHz mono S16 format. */
      source_t() {
        pw_init(nullptr, nullptr);
        loop = pw_thread_loop_new("sunshine-microphone", nullptr);
        if (!loop) {
          return;
        }
        auto props = pw_properties_new(PW_KEY_NODE_VIRTUAL, "true", PW_KEY_NODE_WANT_DRIVER, "true", PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_CLASS, "Audio/Source", PW_KEY_NODE_NAME, source_name, PW_KEY_NODE_DESCRIPTION, "Sunshine Microphone", PW_KEY_AUDIO_RATE, "48000", PW_KEY_AUDIO_CHANNELS, "1", PW_KEY_NODE_LATENCY, "480/48000", nullptr);
        stream = pw_stream_new_simple(pw_thread_loop_get_loop(loop), "Sunshine Microphone", props, &events, this);
        if (!stream) {
          return;
        }
        std::array<uint8_t, 1024> buffer {};
        spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer.data(), buffer.size());
        spa_audio_info_raw format {};
        format.format = SPA_AUDIO_FORMAT_S16_LE;
        format.rate = 48000;
        format.channels = 1;
        format.position[0] = SPA_AUDIO_CHANNEL_MONO;
        const spa_pod *params[] = {spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &format)};
        if (pw_stream_connect(stream, PW_DIRECTION_OUTPUT, PW_ID_ANY, pw_stream_flags(PW_STREAM_FLAG_MAP_BUFFERS), params, 1) < 0 ||
            pw_thread_loop_start(loop) < 0) {
          return;
        }
        running = true;
      }

      /** @brief Disconnecting removes the source when the host exits. */
      ~source_t() {
        if (running) {
          pw_thread_loop_stop(loop);
        }
        if (stream) {
          pw_stream_destroy(stream);
        }
        if (loop) {
          pw_thread_loop_destroy(loop);
        }
      }

      /**
       * @brief Check whether initialization succeeded.
       * @return True if the PipeWire stream and its loop started successfully.
       */
      bool ok() const {
        return running;
      }

      /** @brief Clear queued speech immediately on mute or disconnect, preserving app links. */
      void silence() {
        std::lock_guard lock(mutex);
        pcm.clear();
      }

      /**
       * @brief Enqueue one decoded frame; drop oldest samples on a stalled graph.
       * @param samples One mono frame of frame_samples signed 16-bit samples.
       */
      void write(const int16_t *samples) {
        std::lock_guard lock(mutex);
        pcm.write(samples);
      }

    private:
      /**
       * @brief Deliver graph-requested PCM; zero fill underflow without blocking the graph.
       * @param data Source instance registered with the PipeWire stream callbacks.
       */
      static void process(void *data) {
        auto &self = *static_cast<source_t *>(data);
        auto b = pw_stream_dequeue_buffer(self.stream);
        if (!b) {
          return;
        }
        auto &d = b->buffer->datas[0];
        if (d.data) {
          uint64_t requested = 480;
  #if PW_CHECK_VERSION(0, 3, 50)
          // pw_buffer::requested was added in PipeWire 0.3.50.
          if (b->requested) {
            requested = b->requested;
          }
  #endif
          auto count = std::min<uint64_t>(requested, d.maxsize / sizeof(int16_t));
          auto out = static_cast<int16_t *>(d.data);
          std::lock_guard lock(self.mutex);
          self.pcm.read(out, count);
          d.chunk->offset = 0;
          d.chunk->stride = sizeof(int16_t);
          d.chunk->size = count * sizeof(int16_t);
          b->size = count;
        }
        pw_stream_queue_buffer(self.stream, b);
      }

      inline static const pw_stream_events events {.version = PW_VERSION_STREAM_EVENTS, .process = process};  ///< Stream callbacks.
      pw_thread_loop *loop = nullptr;  ///< Owned PipeWire loop.
      pw_stream *stream = nullptr;  ///< Owned PipeWire stream.
      bool running = false;  ///< Loop lifecycle.
      std::mutex mutex;  ///< Protects the small PCM ring (not held during decode).
      pcm_queue_t pcm;  ///< Bounded PCM queue, protected by mutex.
    };

    /**
     * @brief Keep one silent node across mute/reconnect so recording apps retain their links.
     * @return Shared virtual source, or null if its initialization failed.
     */
    std::shared_ptr<source_t> persistent_source() {
      static std::shared_ptr<source_t> source;
      if (!source) {
        auto created = std::make_shared<source_t>();
        if (created->ok()) {
          source = std::move(created);
        }
      }
      return source;
    }

    /** @brief Separate process restores Pulse default after normal exit or parent SIGKILL. */
    class guardian_t {
    public:
      /** @brief Spawn the same executable in narrowly scoped guardian mode. */
      guardian_t() {
        int fds[2];
        if (pipe2(fds, O_CLOEXEC) < 0) {
          return;
        }
        // Reserve a high descriptor so dup2 always clears CLOEXEC, including when
        // the parent initially had descriptor 3 free.
        int read_fd = fcntl(fds[0], F_DUPFD_CLOEXEC, 10);
        if (read_fd < 0) {
          close(fds[0]);
          close(fds[1]);
          return;
        }
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, read_fd, 3);
  #if defined(__GLIBC__) && __GLIBC_PREREQ(2, 34)
        posix_spawn_file_actions_addclosefrom_np(&actions, 4);
  #endif
        char exe[] = "/proc/self/exe", flag[] = "--microphone-guard", fd[] = "3";
        char *argv[] = {exe, flag, fd, nullptr};
        if (posix_spawn(&pid, exe, &actions, nullptr, argv, environ) != 0) {
          pid = -1;
        }
        close(read_fd);
        posix_spawn_file_actions_destroy(&actions);
        close(fds[0]);
        lifetime_fd = fds[1];
      }

      /** @brief Close the lifetime pipe and reap the bounded guardian operation. */
      ~guardian_t() {
        if (lifetime_fd >= 0) {
          close(lifetime_fd);
        }
        if (pid > 0) {
          while (waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {}
        }
      }

    private:
      int lifetime_fd = -1;  ///< EOF means parent died or microphone stopped.
      pid_t pid = -1;  ///< Owned guardian child.
    };

    /** @brief Decode on a worker; the ENet thread only validates and queues packets. */
    class receiver_impl_t: public receiver_t {
    public:
      /** @brief Start a session-local worker. */
      receiver_impl_t():
          worker([this](std::stop_token stop) {
            run(stop);
          }) {}

      /** @brief Join before releasing the global source lease. */
      ~receiver_impl_t() override {
        worker.request_stop();
        worker.join();
        claimed = false;
      }

      /**
       * @brief Submit a bounded validated packet.
       * @param frame Audio or stop frame to enqueue on the worker.
       */
      void push(frame_t frame) override {
        std::lock_guard lock(mutex);
        queue.push(std::move(frame), clock::now());
      }

    private:
      /**
       * @brief Clock decoding, PLC and source lifecycle at one frame per 20 ms.
       * @param stop Token requesting worker shutdown before session teardown.
       */
      void run(std::stop_token stop) {
        int error;
        auto decoder = opus_decoder_create(48000, 1, &error);
        if (!decoder) {
          return;
        }
        std::shared_ptr<source_t> source;
        std::unique_ptr<guardian_t> guardian;
        auto next = clock::now();
        auto retry = next;
        bool was_active = false;
        while (!stop.stop_requested()) {
          next += std::chrono::milliseconds(20);
          bool live;
          std::optional<std::string> packet;
          {
            std::lock_guard lock(mutex);
            live = queue.live(clock::now());
            if (live) {
              packet = queue.pop(clock::now());
            } else {
              queue.clear();
            }
          }
          if (live && !source && clock::now() >= retry) {
            source = persistent_source();
            if (!source) {
              retry = clock::now() + std::chrono::seconds(2);
            }
          }
          if (live && source && !guardian) {
            guardian = std::make_unique<guardian_t>();
            BOOST_LOG(info) << "Phone microphone source active";
          }
          if (!live && was_active && source) {
            source->silence();
            opus_decoder_ctl(decoder, OPUS_RESET_STATE);
            BOOST_LOG(info) << "Phone microphone source silent";
          }
          was_active = live;
          if (packet && source) {
            std::array<int16_t, frame_samples> pcm {};
            int samples = opus_decode(decoder, packet->empty() ? nullptr : reinterpret_cast<const unsigned char *>(packet->data()), packet->size(), pcm.data(), pcm.size(), 0);
            if (samples != frame_samples) {
              pcm.fill(0);
            }
            source->write(pcm.data());
          }
          // Never catch up by emitting a burst after scheduler suspension.
          if (next < clock::now()) {
            next = clock::now();
          }
          std::this_thread::sleep_until(next);
        }
        if (source) {
          source->silence();
        }
        guardian.reset();
        source.reset();
        opus_decoder_destroy(decoder);
      }

      std::mutex mutex;  ///< Protects network-to-worker queue.
      jitter_t queue;  ///< Bounded encrypted audio payload queue.
      std::jthread worker;  ///< Joined before session teardown.
    };
  }  // namespace

  bool available() {
    return true;
  }

  std::unique_ptr<receiver_t> create() {
    if (claimed.exchange(true)) {
      return nullptr;
    }
    return std::make_unique<receiver_impl_t>();
  }

  int guard(int fd) {
    if (fd != 3 || fcntl(fd, F_GETFD) < 0) {
      return 1;
    }
    pulse_t pulse;
    if (!pulse.connect() || !pulse.query()) {
      return 1;
    }
    auto previous = pulse.current;
    if (previous == source_name) {
      return 1;
    }
    bool owns = false;
    auto end = clock::now() + std::chrono::seconds(3);
    pollfd pipe {fd, POLLIN | POLLHUP, 0};
    while (poll(&pipe, 1, 50) == 0) {
      if (!pulse.query()) {
        break;
      }
      if (!owns && pulse.exists) {
        owns = pulse.set(source_name);
        if (!owns) {
          break;
        }
      } else if (owns && pulse.exists && pulse.current != source_name) {
        owns = false;
        break;
      }
      if (!owns && clock::now() > end) {
        break;
      }
    }
    if (owns) {
      pulse.set(previous);
    }
    close(fd);
    return 0;
  }
}  // namespace microphone
#else
namespace microphone {
  bool available() {
    return false;
  }

  std::unique_ptr<receiver_t> create() {
    return nullptr;
  }

  int guard(int) {
    return 1;
  }
}  // namespace microphone
#endif
