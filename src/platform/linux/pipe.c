#include <pipewire/extensions/metadata.h>
#include <spa/utils/json.h>
#include <stdatomic.h>
#include <stddefer.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pipe.h"
#include "platform/platform.h"

typedef struct {
  uint32_t id;
  // a duplex device is both an input and an output.
  bool kinds[3];
  bool own;
  char *name, *description;
} Node;

static struct {
  struct pw_thread_loop *loop;
  struct pw_context *context;
  struct pw_core *core;
  struct pw_registry *registry;
  struct pw_metadata *metadata;
  uint32_t metadata_id;
  struct spa_hook core_listener, registry_listener, metadata_listener;
  int synced;
  bool broken;
  int holders;

  Node *nodes;
  size_t nnodes, cap;
  // the node names of the system's own devices, by kind.
  char defaults[3][256];
  void (*on_nodes)(void);
} P;

static const char *const DEFAULT_KEYS[3] = {
  [AUDIO_INPUTS] = "default.audio.source",
  [AUDIO_OUTPUTS] = "default.audio.sink",
  [AUDIO_CAMERAS] = "default.video.source",
};

// the device list changed: told to the main thread once, however many
// changes came before it looked.
static uint32_t devices_version;
static void (*devices_changed)(void);
static atomic_bool moved_posted;

static void moved_on_main(void *) {
  atomic_store(&moved_posted, false);
  devices_version++;

  if (devices_changed != nullptr) {
    devices_changed();
  }
}

static void moved(void) {
  if (P.on_nodes != nullptr) {
    P.on_nodes();
  }

  if (!atomic_exchange(&moved_posted, true)) {
    platform_post(moved_on_main, nullptr);
  }
}

static void forget_nodes(void) {
  for (size_t i = 0; i < P.nnodes; i++) {
    free(P.nodes[i].name);
    free(P.nodes[i].description);
  }

  P.nnodes = 0;
  memset(P.defaults, 0, sizeof P.defaults);
}

static int on_property(void *, uint32_t subject, const char *key, const char *,
                       const char *value) {
  if (subject != PW_ID_CORE) {
    return 0;
  }

  for (int kind = 0; kind < 3; kind++) {
    if (key == nullptr || strcmp(key, DEFAULT_KEYS[kind]) == 0) {
      char name[256] = "";

      // a value like { "name": "alsa_output.pci..." }, none when unset.
      if (value != nullptr) {
        spa_json_str_object_find(value, strlen(value), "name", name,
                                 sizeof name);
      }

      snprintf(P.defaults[kind], sizeof P.defaults[kind], "%s", name);
    }
  }

  moved();
  return 0;
}

static const struct pw_metadata_events metadata_events = {
  PW_VERSION_METADATA_EVENTS,
  .property = on_property,
};

static void on_global(void *, uint32_t id, uint32_t, const char *type, uint32_t,
                      const struct spa_dict *props) {
  if (props == nullptr) {
    return;
  }

  if (strcmp(type, PW_TYPE_INTERFACE_Metadata) == 0) {
    const char *name = spa_dict_lookup(props, PW_KEY_METADATA_NAME);

    if (P.metadata == nullptr && name != nullptr &&
        strcmp(name, "default") == 0) {
      P.metadata =
        pw_registry_bind(P.registry, id, type, PW_VERSION_METADATA, 0);
      P.metadata_id = id;
      pw_metadata_add_listener(P.metadata, &P.metadata_listener,
                               &metadata_events, nullptr);
    }

    return;
  }

  if (strcmp(type, PW_TYPE_INTERFACE_Node) != 0) {
    return;
  }

  const char *class = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
  const char *name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
  const char *description = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION);

  if (class == nullptr || name == nullptr) {
    return;
  }

  Node node = {.id = id, .own = strncmp(name, PIPE_OWN, strlen(PIPE_OWN)) == 0};

  node.kinds[AUDIO_INPUTS] = strncmp(class, "Audio/Source", 12) == 0 ||
                             strcmp(class, "Audio/Duplex") == 0;
  node.kinds[AUDIO_OUTPUTS] =
    strcmp(class, "Audio/Sink") == 0 || strcmp(class, "Audio/Duplex") == 0;
  node.kinds[AUDIO_CAMERAS] = strcmp(class, "Video/Source") == 0;

  if (!node.kinds[0] && !node.kinds[1] && !node.kinds[2]) {
    return;
  }

  if (P.nnodes == P.cap) {
    size_t cap = P.cap ? P.cap * 2 : 16;
    Node *grown = realloc(P.nodes, cap * sizeof *grown);

    if (grown == nullptr) {
      return;
    }

    P.nodes = grown;
    P.cap = cap;
  }

  node.name = strdup(name);
  node.description = strdup(description ? description : name);
  P.nodes[P.nnodes++] = node;
  moved();
}

static void on_global_remove(void *, uint32_t id) {
  if (P.metadata != nullptr && id == P.metadata_id) {
    spa_hook_remove(&P.metadata_listener);
    pw_proxy_destroy((struct pw_proxy *)P.metadata);
    P.metadata = nullptr;
    memset(P.defaults, 0, sizeof P.defaults);
    moved();
    return;
  }

  for (size_t i = 0; i < P.nnodes; i++) {
    if (P.nodes[i].id == id) {
      free(P.nodes[i].name);
      free(P.nodes[i].description);
      P.nodes[i] = P.nodes[--P.nnodes];
      moved();
      return;
    }
  }
}

static const struct pw_registry_events registry_events = {
  PW_VERSION_REGISTRY_EVENTS,
  .global = on_global,
  .global_remove = on_global_remove,
};

static void on_done(void *, uint32_t id, int seq) {
  if (id == PW_ID_CORE && seq == P.synced) {
    P.synced = -1;
    pw_thread_loop_signal(P.loop, false);
  }
}

static void on_error(void *, uint32_t id, int, int res, const char *message) {
  if (id != PW_ID_CORE || res != -EPIPE) {
    return;
  }

  fprintf(stderr, "quarrel: pipewire went away: %s\n", message);
  P.broken = true;
  forget_nodes();
  moved();

  // a start waiting for the first answer gives up.
  P.synced = -1;
  pw_thread_loop_signal(P.loop, false);
}

static const struct pw_core_events core_events = {
  PW_VERSION_CORE_EVENTS,
  .done = on_done,
  .error = on_error,
};

static void disconnect(void) {
  if (P.metadata != nullptr) {
    spa_hook_remove(&P.metadata_listener);
    pw_proxy_destroy((struct pw_proxy *)P.metadata);
    P.metadata = nullptr;
  }

  if (P.registry != nullptr) {
    spa_hook_remove(&P.registry_listener);
    pw_proxy_destroy((struct pw_proxy *)P.registry);
    P.registry = nullptr;
  }

  if (P.core != nullptr) {
    spa_hook_remove(&P.core_listener);
    pw_core_disconnect(P.core);
    P.core = nullptr;
  }

  forget_nodes();
}

// with the lock held: the core, its registry and every global it has seen.
static bool connect_core(void) {
  P.core = pw_context_connect(P.context, nullptr, 0);

  if (P.core == nullptr) {
    return false;
  }

  P.broken = false;
  pw_core_add_listener(P.core, &P.core_listener, &core_events, nullptr);
  P.registry = pw_core_get_registry(P.core, PW_VERSION_REGISTRY, 0);
  pw_registry_add_listener(P.registry, &P.registry_listener, &registry_events,
                           nullptr);

  // the first answer has every global, the second what the metadata bound
  // meanwhile holds.
  for (int round = 0; round < 2 && !P.broken; round++) {
    P.synced = pw_core_sync(P.core, PW_ID_CORE, 0);

    while (P.synced != -1) {
      pw_thread_loop_wait(P.loop);
    }
  }

  if (P.broken) {
    disconnect();
    return false;
  }

  return true;
}

bool pipe_start(void) {
  if (P.loop == nullptr) {
    pw_init(nullptr, nullptr);
    P.loop = pw_thread_loop_new("quarrel-pipewire", nullptr);

    if (P.loop == nullptr) {
      return false;
    }

    P.context =
      pw_context_new(pw_thread_loop_get_loop(P.loop),
                     pw_properties_new(PW_KEY_APP_NAME, "Quarrel",
                                       PW_KEY_APP_ID, "quarrel", nullptr),
                     0);

    if (P.context == nullptr || pw_thread_loop_start(P.loop) != 0) {
      P.context = nullptr;
      return false;
    }
  }

  if (P.context == nullptr) {
    return false;
  }

  pipe_lock();

  defer pipe_unlock();

  if (P.core != nullptr && P.broken && P.holders == 0) {
    disconnect();
  }

  return P.core != nullptr ? !P.broken : connect_core();
}

struct pw_thread_loop *pipe_loop(void) {
  return P.loop;
}

struct pw_context *pipe_context(void) {
  return P.context;
}

struct pw_core *pipe_core(void) {
  return P.core;
}

void pipe_lock(void) {
  pw_thread_loop_lock(P.loop);
}

void pipe_unlock(void) {
  pw_thread_loop_unlock(P.loop);
}

typedef struct {
  void (*fn)(void *ctx);
  void *ctx;
} Run;

static int run_invoked(struct spa_loop *, bool, uint32_t, const void *data,
                       size_t, void *) {
  const Run *run = data;

  run->fn(run->ctx);
  return 0;
}

void pipe_run(void (*fn)(void *ctx), void *ctx) {
  Run run = {fn, ctx};

  pw_loop_invoke(pw_thread_loop_get_loop(P.loop), run_invoked, 0, &run,
                 sizeof run, false, nullptr);
}

bool pipe_has_node(const char *name) {
  for (size_t i = 0; i < P.nnodes; i++) {
    if (strcmp(P.nodes[i].name, name) == 0) {
      return true;
    }
  }

  return false;
}

bool pipe_is_device(AudioKind kind, const char *name) {
  for (size_t i = 0; name != nullptr && i < P.nnodes; i++) {
    if (!P.nodes[i].own && P.nodes[i].kinds[kind] &&
        strcmp(P.nodes[i].name, name) == 0) {
      return true;
    }
  }

  return false;
}

void pipe_on_nodes(void (*fn)(void)) {
  P.on_nodes = fn;
}

void pipe_hold(void) {
  P.holders++;
}

void pipe_release(void) {
  P.holders--;
}

size_t audio_devices(AudioKind kind, AudioDevice **out) {
  size_t count = 0;

  *out = nullptr;

  if (!pipe_start()) {
    return 0;
  }

  pipe_lock();

  defer pipe_unlock();

  AudioDevice *list = calloc(P.nnodes ? P.nnodes : 1, sizeof *list);

  if (list == nullptr) {
    return 0;
  }

  for (size_t i = 0; i < P.nnodes; i++) {
    const Node *node = &P.nodes[i];

    if (!node->own && node->kinds[kind]) {
      list[count++] =
        (AudioDevice){strdup(node->name), strdup(node->description)};
    }
  }

  *out = list;
  return count;
}

void audio_devices_free(AudioDevice *devices, size_t count) {
  for (size_t i = 0; i < count; i++) {
    free(devices[i].id);
    free(devices[i].name);
  }

  free(devices);
}

uint32_t audio_devices_version(void (*changed)(void)) {
  if (devices_changed == nullptr) {
    devices_changed = changed;
    pipe_start();
  }

  return devices_version;
}

void audio_default_name(AudioKind kind, char *out, size_t cap) {
  out[0] = '\0';

  if (!pipe_start()) {
    return;
  }

  pipe_lock();

  defer pipe_unlock();

  const Node *first = nullptr;

  for (size_t i = 0; i < P.nnodes; i++) {
    const Node *node = &P.nodes[i];

    if (node->own || !node->kinds[kind]) {
      continue;
    }

    if (strcmp(node->name, P.defaults[kind]) == 0) {
      snprintf(out, cap, "%s", node->description);
      return;
    }

    first = first ? first : node;
  }

  // a camera is seldom made the default, the first one is then.
  if (kind == AUDIO_CAMERAS && first != nullptr) {
    snprintf(out, cap, "%s", first->description);
  }
}
