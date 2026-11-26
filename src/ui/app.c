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

bool app_revealed(const App *app, civue_id spoiler) {
  for (uint32_t i = 0; i < app->revealed.len; i++) {
    if (app->revealed.v[i] == spoiler) {
      return true;
    }
  }

  return false;
}

void app_reveal(App *app, civue_id spoiler) {
  if (app->revealed.len == app->revealed.cap) {
    app->revealed.cap = app->revealed.cap ? app->revealed.cap * 2 : 16;
    app->revealed.v =
      realloc(app->revealed.v, app->revealed.cap * sizeof *app->revealed.v);
  }

  app->revealed.v[app->revealed.len++] = spoiler;
}

void app_reset(App *app) {
  app->account_id = app->session->account_id;
  app->popover = POPOVER_NONE;
  app->guild_id = 0;
  app->last_channels.len = 0;
  app->revealed.len = 0;
  app->hide_muted.len = 0;
  app->picks.len = 0;
  app->forum_id = 0;
  app->forum_tags.len = 0;
  app->post_tags.len = 0;
  app->post_open = false;

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
  app->fake_ask = false;
  app->upload_title[0] = 0;
  drop(&app->gif_search);
  drop(&app->card_emoji_name);
  drop(&app->gif_title);
  drop(&app->sticker_search);
  app->sticker_folded.len = 0;
  app->sticker_pending = 0;
  app->sticker_menu = 0;
  drop(&app->forum_search);
  drop(&app->post_title);
  drop(&app->post_body);
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

  drop(&app->directory_field);
  drop(&app->directory_query);
  drop(&app->new_server_name);
  drop(&app->entry_description);
  drop(&app->hub_email);
  drop(&app->hub_code);
  drop(&app->hub_school);
  drop(&app->hub_search);
  drop(&app->nick);

  app->directory_tab = -1;
  app->directory_join = 0;
  app->hub_dialog = HUB_DIALOG_NONE;
  app->nick_guild = 0;

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

  // a directory opens on its home, its search forgotten.
  if (channel_id != app->session->selected_channel_id) {
    free(app->directory_query);
    free(app->directory_field);
    app->directory_query = nullptr;
    app->directory_field = nullptr;
    app->directory_tab = -1;
  }

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

  // a hub opens on the servers it lists.
  if (!store_channel(store, channel_id) && guild && guild->hub) {
    Channel *directory = store_guild_directory(store, guild);

    channel_id = directory ? directory->id : 0;
  }

  if (!store_channel(store, channel_id)) {
    channel_id = guild ? first_channel(app, guild) : 0;
  }

  app_select_channel(app, channel_id);
  app->guild_id = guild_id;
}
