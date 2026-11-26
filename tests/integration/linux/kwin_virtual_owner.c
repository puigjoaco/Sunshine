/**
 * @file tests/integration/linux/kwin_virtual_owner.c
 * @brief Private nested-KWin protocol owner for exclusive recovery checks.
 */
#include "zkde-screencast-unstable-v1.h"

#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-client.h>

static struct zkde_screencast_unstable_v1 *manager;

static void global(void *data, struct wl_registry *registry, uint32_t id, const char *interface, uint32_t version) {
  if (!strcmp(interface, "zkde_screencast_unstable_v1")) {
    manager = wl_registry_bind(registry, id, &zkde_screencast_unstable_v1_interface, version < 4 ? version : 4);
  }
}

static void removed(void *data, struct wl_registry *registry, uint32_t id) {}
static const struct wl_registry_listener listener = {global, removed};

int main(int argc, char **argv) {
  if (argc != 4) {
    return 2;
  }
  struct wl_display *display = wl_display_connect(NULL);
  if (!display) {
    return 3;
  }
  struct wl_registry *registry = wl_display_get_registry(display);
  wl_registry_add_listener(registry, &listener, NULL);
  wl_display_roundtrip(display);
  if (!manager) {
    return 4;
  }
  struct zkde_screencast_stream_unstable_v1 *stream = zkde_screencast_unstable_v1_stream_virtual_output(manager, argv[1], atoi(argv[2]), atoi(argv[3]), wl_fixed_from_int(1), 2);
  wl_display_flush(display);
  /* Do not consume unhandled screencast events; just keep the ownership socket. */
  char byte;
  while (read(STDIN_FILENO, &byte, 1) > 0) {}
  zkde_screencast_stream_unstable_v1_close(stream);
  wl_display_flush(display);
  wl_display_disconnect(display);
  return 0;
}
