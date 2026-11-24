#include <glib/gstdio.h>
#include <libsecret/secret.h>
#include <stddefer.h>
#include <stdlib.h>
#include <string.h>

#include "platform/platform.h"

// a write waits this long for the ones after it, a slider sets a pref at
// every step it moves.
constexpr guint SAVE_AFTER_MS = 1000;

static const SecretSchema schema = {
  .name = "com.vexcited.quarrel",
  .flags = SECRET_SCHEMA_NONE,
  .attributes = {{"account", SECRET_SCHEMA_ATTRIBUTE_STRING}},
};

bool platform_secret_set(const char *key, const char *value) {
  if (key == nullptr || value == nullptr) {
    return false;
  }

  char *label = g_strdup_printf("Quarrel %s", key);

  defer g_free(label);

  return secret_password_store_sync(&schema, SECRET_COLLECTION_DEFAULT, label,
                                    value, nullptr, nullptr, "account", key,
                                    nullptr);
}

char *platform_secret_get(const char *key, bool *missing) {
  GError *error = nullptr;

  if (missing != nullptr) {
    *missing = false;
  }

  if (key == nullptr) {
    return nullptr;
  }

  char *found = secret_password_lookup_sync(&schema, nullptr, &error, "account",
                                            key, nullptr);

  if (found == nullptr) {
    if (missing != nullptr) {
      *missing = error == nullptr;
    }

    g_clear_error(&error);
    return nullptr;
  }

  char *value = strdup(found);

  secret_password_free(found);
  return value;
}

bool platform_secret_delete(const char *key) {
  GError *error = nullptr;

  if (key == nullptr) {
    return false;
  }

  secret_password_clear_sync(&schema, nullptr, &error, "account", key, nullptr);

  bool cleared = error == nullptr;

  g_clear_error(&error);
  return cleared;
}

static GKeyFile *prefs;
static char *prefs_path;
static guint save_source;

static void save(void) {
  if (save_source != 0) {
    g_source_remove(save_source);
    save_source = 0;
  }

  gsize length = 0;
  char *data = g_key_file_to_data(prefs, &length, nullptr);
  char *dir = g_path_get_dirname(prefs_path);

  defer g_free(data);
  defer g_free(dir);

  if (g_mkdir_with_parents(dir, 0o700) == 0) {
    g_file_set_contents_full(prefs_path, data, (gssize)length,
                             G_FILE_SET_CONTENTS_CONSISTENT, 0o600, nullptr);
  }
}

static void save_later(void *) {
  save_source = 0;
  save();
}

static void save_now(void) {
  if (save_source != 0) {
    save();
  }
}

static GKeyFile *load(void) {
  if (prefs != nullptr) {
    return prefs;
  }

  prefs = g_key_file_new();
  prefs_path = g_build_filename(g_get_user_config_dir(), "quarrel",
                                "preferences", nullptr);
  g_key_file_load_from_file(prefs, prefs_path, G_KEY_FILE_KEEP_COMMENTS,
                            nullptr);
  atexit(save_now);
  return prefs;
}

void platform_pref_set(const char *key, const char *value) {
  if (key == nullptr) {
    return;
  }

  GKeyFile *file = load();
  char *was = g_key_file_get_string(file, "quarrel", key, nullptr);

  defer g_free(was);

  if (value == nullptr ? was == nullptr
                       : was != nullptr && strcmp(was, value) == 0) {
    return;
  }

  if (value == nullptr) {
    g_key_file_remove_key(file, "quarrel", key, nullptr);
  }
  else {
    g_key_file_set_string(file, "quarrel", key, value);
  }

  if (save_source == 0) {
    save_source = g_timeout_add_once(SAVE_AFTER_MS, save_later, nullptr);
  }
}

char *platform_pref_get(const char *key) {
  if (key == nullptr) {
    return nullptr;
  }

  char *value = g_key_file_get_string(load(), "quarrel", key, nullptr);
  char *copy = value != nullptr ? strdup(value) : nullptr;

  g_free(value);
  return copy;
}
