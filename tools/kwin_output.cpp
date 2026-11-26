/**
 * @file tools/kwin_output.cpp
 * @brief Bounded, replication-only KWin output-management v2 transactions.
 *
 * Accepts connector=source-connector pairs; an empty source clears mirroring.
 * Never changes enabled state, DPMS, modes, positions or scale. A guardian uses
 * it because kscreen-doctor exposes replication in JSON but cannot configure it.
 */
#include <algorithm>
#include <csignal>
#include <cstring>
#include <iostream>
#include <kde-output-device-v2.h>
#include <kde-output-management-v2.h>
#include <memory>
#include <string>
#include <unistd.h>
#include <vector>
#include <wayland-client.h>

namespace {
  /** @brief Device identity and actual replication source from compositor events. */
  struct output_t {
    kde_output_device_v2 *proxy = nullptr;  ///< Versioned output device.
    std::string name;  ///< Connector name used by the guardian.
    std::string uuid;  ///< Compositor identity used by the replication request.
    std::string source;  ///< Currently applied source UUID, or empty.
  };

  /** @brief Own discovery and one atomic transaction for this short-lived process. */
  struct state_t {
    kde_output_management_v2 *manager = nullptr;  ///< Output-management global.
    std::vector<std::unique_ptr<output_t>> outputs;  ///< Stable event user data.
    bool finished = false;  ///< Configuration result received.
    bool applied = false;  ///< Compositor accepted the transaction.
  };

  /** @brief Consume mode events; this tool deliberately never selects modes. */
  int ignore_event(const void *, void *, uint32_t, const wl_message *, wl_argument *) {
    return 0;
  }

  /** @brief Read just connector identities and replication, consuming all events. */
  int output_event(const void *, void *proxy, uint32_t, const wl_message *message, wl_argument *args) {
    auto &output = *static_cast<output_t *>(wl_proxy_get_user_data(static_cast<wl_proxy *>(proxy)));
    if (std::strcmp(message->name, "name") == 0) {
      output.name = args[0].s;
    } else if (std::strcmp(message->name, "uuid") == 0) {
      output.uuid = args[0].s;
    } else if (std::strcmp(message->name, "replication_source") == 0) {
      output.source = args[0].s;
    } else if (std::strcmp(message->name, "mode") == 0) {
      wl_proxy_add_dispatcher(reinterpret_cast<wl_proxy *>(args[0].o), ignore_event, nullptr, nullptr);
    }
    return 0;
  }

  /** @brief Bind only protocol versions that can express replication. */
  void global(void *data, wl_registry *registry, uint32_t id, const char *interface, uint32_t version) {
    auto &state = *static_cast<state_t *>(data);
    if (std::strcmp(interface, "kde_output_management_v2") == 0 && version >= 13) {
      state.manager = static_cast<kde_output_management_v2 *>(wl_registry_bind(registry, id, &kde_output_management_v2_interface, std::min(version, 22u)));
    } else if (std::strcmp(interface, "kde_output_device_v2") == 0 && version >= 13) {
      auto output = std::make_unique<output_t>();
      output->proxy = static_cast<kde_output_device_v2 *>(wl_registry_bind(registry, id, &kde_output_device_v2_interface, std::min(version, 22u)));
      wl_proxy_add_dispatcher(reinterpret_cast<wl_proxy *>(output->proxy), output_event, nullptr, output.get());
      state.outputs.push_back(std::move(output));
    }
  }

  /** @brief Discovery is short-lived; stale/missing devices fail transaction lookup. */
  void removed(void *, wl_registry *, uint32_t) {}

  /** @brief Consume the result and preserve the server's rejection explanation. */
  int configuration_event(const void *, void *proxy, uint32_t, const wl_message *message, wl_argument *args) {
    auto &state = *static_cast<state_t *>(wl_proxy_get_user_data(static_cast<wl_proxy *>(proxy)));
    if (std::strcmp(message->name, "applied") == 0 || std::strcmp(message->name, "failed") == 0) {
      state.finished = true;
      state.applied = std::strcmp(message->name, "applied") == 0;
    } else if (std::strcmp(message->name, "failure_reason") == 0) {
      std::cerr << "KWin replication: " << args[0].s << '\n';
    }
    return 0;
  }
}  // namespace

/**
 * @brief Apply connector=source pairs and verify the source UUID readback.
 * @param argc Number of connector mappings (or --probe).
 * @param argv Connector mappings, with empty source to clear replication.
 * @return Zero only when all requests are accepted and verified.
 */
int main(int argc, char **argv) {
  if (argc < 2) {
    return 2;
  }
  alarm(4);  // Even an unresponsive compositor cannot leave this helper hanging.
  auto *display = wl_display_connect(nullptr);
  if (!display) {
    return 3;
  }
  state_t state;
  auto *registry = wl_display_get_registry(display);
  static constexpr wl_registry_listener listener {global, removed};
  wl_registry_add_listener(registry, &listener, &state);
  if (wl_display_roundtrip(display) < 0 || wl_display_roundtrip(display) < 0 || !state.manager) {
    return 4;
  }
  if (argc == 2 && std::strcmp(argv[1], "--probe") == 0) {
    return 0;
  }
  auto find = [&](const std::string &name) -> output_t * {
    for (const auto &output : state.outputs) {
      if (output->name == name && !output->uuid.empty()) {
        return output.get();
      }
    }
    return nullptr;
  };
  auto *configuration = kde_output_management_v2_create_configuration(state.manager);
  wl_proxy_add_dispatcher(reinterpret_cast<wl_proxy *>(configuration), configuration_event, nullptr, &state);
  std::vector<std::pair<output_t *, std::string>> desired;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    const auto separator = arg.find('=');
    if (separator == std::string::npos) {
      return 2;
    }
    auto *sink = find(arg.substr(0, separator));
    const auto source_name = arg.substr(separator + 1);
    auto *source = source_name.empty() ? nullptr : find(source_name);
    if (!sink || (!source_name.empty() && !source) || sink == source) {
      return 5;
    }
    const auto uuid = source ? source->uuid : std::string {};
    kde_output_configuration_v2_set_replication_source(configuration, sink->proxy, uuid.c_str());
    desired.emplace_back(sink, uuid);
  }
  kde_output_configuration_v2_apply(configuration);
  while (!state.finished) {
    if (wl_display_dispatch(display) < 0) {
      return 6;
    }
  }
  if (!state.applied || wl_display_roundtrip(display) < 0) {
    return 7;
  }
  const bool matches = std::all_of(desired.begin(), desired.end(), [](const auto &row) {
    return row.first->source == row.second;
  });
  wl_display_disconnect(display);
  return matches ? 0 : 8;
}
