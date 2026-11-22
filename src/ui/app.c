#include "app.h"

#include <stdlib.h>
#include <string.h>

Channel *app_channel(App *app) {
  Snowflake id = app->session->selected_channel_id;

  return id ? store_channel(app->session->store, id) : nullptr;
}

Guild *app_guild(App *app) {
  return app->guild_id ? store_guild(app->session->store, app->guild_id)
                       : nullptr;
}

static LastChannel *last_of(App *app, Snowflake guild_id) {
  for (uint32_t i = 0; i < app->last_channels.len; i++) {
    if (app->last_channels.v[i].guild_id == guild_id) {
      return &app->last_channels.v[i];
    }
  }

  if (app->last_channels.len == app->last_channels.cap) {
    uint32_t cap = app->last_channels.cap ? app->last_channels.cap * 2 : 8;

    app->last_channels.v =
      realloc(app->last_channels.v, cap * sizeof *app->last_channels.v);
    app->last_channels.cap = cap;
  }

  LastChannel *last = &app->last_channels.v[app->last_channels.len++];
  *last = (LastChannel){.guild_id = guild_id};

  return last;
}

static void drop(char **text) {
  free(*text);
  *text = nullptr;
}

bool ids_has(const Ids *ids, Snowflake id) {
  for (uint32_t i = 0; i < ids->len; i++) {
    if (ids->v[i] == id) {
      return true;
    }
  }

  return false;
}

void ids_toggle(Ids *ids, Snowflake id) {
  for (uint32_t i = 0; i < ids->len; i++) {
    if (ids->v[i] == id) {
      ids->v[i] = ids->v[--ids->len];
      return;
    }
  }

  if (ids->len == ids->cap) {
    ids->cap = ids->cap ? ids->cap * 2 : 8;
    ids->v = realloc(ids->v, ids->cap * sizeof *ids->v);
  }

  ids->v[ids->len++] = id;
}

void app_reset(App *app) {
  app->account_id = app->session->account_id;
  app->popover = POPOVER_NONE;
  app->guild_id = 0;
  app->last_channels.len = 0;
  app->revealed.len = 0;
  app->hide_muted.len = 0;
  app->picks.len = 0;

  // the password does not stay around once it served.
  if (app->password) {
    memset(app->password, 0, strlen(app->password));
  }

  drop(&app->login);
  drop(&app->password);
  drop(&app->code);
  drop(&app->draft);
  drop(&app->kept_draft);
  drop(&app->friends_search);
  drop(&app->friend_tag);
  drop(&app->friend_note);
  drop(&app->dm_search);
  drop(&app->pick_search);
  drop(&app->guild_name);
  drop(&app->menu_search);
  drop(&app->upload_name);
  drop(&app->upload_alt);
  app->upload_editing = 0;
  app->upload_menu = false;
  app->long_message = false;
  app->upload_title[0] = 0;
  drop(&app->profile_message);
  drop(&app->profile_note);

  app->profile_user = 0;
  app->modal_user = 0;
  app->note_user = 0;

  app->friend_ready = false;

  if (app->friend_captcha) {
    hcaptcha_free(app->friend_captcha);
    app->friend_captcha = nullptr;
  }

  app->reply_to = 0;
  app->editing = 0;
  app->list_channel = 0;
  app->stick = true;
  app->hover_message = 0;
  app->hover_next = 0;
  app->friends_tab = FRIENDS_ONLINE;

  // the account comes back where it was left.
  Channel *channel = app_channel(app);

  if (channel) {
    app->guild_id = channel->guild_id;
    last_of(app, app->guild_id)->channel_id = channel->id;
  }
}

static void leave_composer(App *app) {
  app->reply_to = 0;

  if (app->editing) {
    app->editing = 0;
    free(app->draft);
    app->draft = app->kept_draft;
    app->kept_draft = nullptr;
  }
}

void app_select_channel(App *app, Snowflake channel_id) {
  Channel *channel = store_channel(app->session->store, channel_id);

  if (channel_id && !channel) {
    return;
  }

  leave_composer(app);
  app->guild_id = channel ? channel->guild_id : 0;
  last_of(app, app->guild_id)->channel_id = channel_id;
  app->focus_composer = channel_id != 0;

  session_select_channel(app->session, channel_id);
}

// the first channel of a guild with messages to read.
static Snowflake first_channel(App *app, Guild *guild) {
  size_t count = 0;
  const ChannelRow *rows =
    store_guild_channels(app->session->store, guild, &count);

  for (size_t i = 0; i < count; i++) {
    const Channel *c = rows[i].channel;

    if (rows[i].kind == CHANNEL_ROW_CHANNEL && channel_has_text(c) &&
        !channel_is_voice(c)) {
      return c->id;
    }
  }

  return 0;
}

void app_select_guild(App *app, Snowflake guild_id) {
  Store *store = app->session->store;
  Guild *guild = guild_id ? store_guild(store, guild_id) : nullptr;
  Snowflake channel_id = last_of(app, guild_id)->channel_id;

  if (guild_id && !guild) {
    return;
  }

  if (!store_channel(store, channel_id)) {
    channel_id = guild ? first_channel(app, guild) : 0;
  }

  app_select_channel(app, channel_id);
  app->guild_id = guild_id;
}
