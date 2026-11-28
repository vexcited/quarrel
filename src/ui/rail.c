#include "components.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <civue/dsl.h>

#include "discord/cdn.h"
#include "widgets.h"

constexpr float TILE = 48;
// a folder's tile pictures four of its guilds this big.
constexpr float MINI = 18;
// out of a folder faster than in.
constexpr float SLIDE_IN_SECONDS = .24f;
constexpr float SLIDE_OUT_SECONDS = .2f;
constexpr float PILL_SECONDS = .14f;
// a held tile sinks in by this on each side, as a pressed button scales to
// about 0.95.
constexpr float PRESS_INSET = 1;
constexpr float PRESS_SECONDS = .12f;
// the pills at a list's edges come in a little slower than they go.
constexpr float BEYOND_IN_SECONDS = .18f;
constexpr float BEYOND_OUT_SECONDS = .12f;
// how far the edge of a list fades where it hides more.
constexpr int FADE = 16;
// the frames the rail takes to bring a guild opened elsewhere in view.
constexpr uint8_t REVEAL = 4;
// the conversations with something new, under the messages: how many at
// most as discord, how fast one comes and goes, and how big it grows from.
constexpr size_t DMS = 20;
constexpr float DM_IN_SECONDS = .2f;
constexpr float DM_OUT_SECONDS = .15f;
constexpr float DM_FROM = .5f;
// between two tiles of a list.
constexpr float GAP = 8;
// the card of the tile under the pointer grows out of its arrow from this
// big, as a popover comes, and goes faster.
constexpr float CARD_FROM = .92f;
constexpr float CARD_IN_SECONDS = .18f;
constexpr float CARD_OUT_SECONDS = .12f;
constexpr float CARD_WIDTH = 280;
// how long it waits for the pointer between two tiles, as long as a tooltip
// shows at once after another.
constexpr double CARD_GAP_SECONDS = .3;
// the faces of who is in voice on a card, and how many before a count.
constexpr float FACE = 24;
constexpr size_t FACES = 5;

static Snowflake folder_key(const GuildFolder *f) {
  return f->id ? f->id : f->guilds[0]->id;
}

// a folder of the user's, not a guild on its own.
static bool is_folder(const GuildFolder *f) {
  return f->nguilds && (f->id != 0 || f->nguilds > 1);
}

static const GuildFolder *find_folder(App *app, Snowflake key) {
  size_t count = 0;
  const GuildFolder *folders = store_guild_folders(app->session->store, &count);

  for (size_t i = 0; key && i < count; i++) {
    if (is_folder(&folders[i]) && folder_key(&folders[i]) == key) {
      return &folders[i];
    }
  }

  return nullptr;
}

static const GuildFolder *folder_of(App *app, Snowflake guild_id) {
  size_t count = 0;
  const GuildFolder *folders = store_guild_folders(app->session->store, &count);

  for (size_t i = 0; guild_id && i < count; i++) {
    for (size_t j = 0; is_folder(&folders[i]) && j < folders[i].nguilds; j++) {
      if (folders[i].guilds[j]->id == guild_id) {
        return &folders[i];
      }
    }
  }

  return nullptr;
}

// a folder's name, its guilds' when it has none, as discord shows it.
static void folder_name(const GuildFolder *f, char *out, size_t cap) {
  size_t n = 0;

  out[0] = 0;

  if (f->name && *f->name) {
    snprintf(out, cap, "%s", f->name);
    return;
  }

  for (size_t i = 0; i < f->nguilds && n + 1 < cap; i++) {
    const char *name = f->guilds[i]->name;

    n += (size_t)snprintf(out + n, cap - n, "%s%s", i ? ", " : "",
                          name ? name : "");
  }
}

bool guild_folder_name(App *app, Snowflake guild_id, char *out, size_t cap) {
  const GuildFolder *f = folder_of(app, guild_id);

  out[0] = 0;

  if (f) {
    folder_name(f, out, cap);
  }

  return f;
}

static civue_color folder_tint(App *app, const GuildFolder *f) {
  return f->has_color ? rgb(f->color) : app->look.primary;
}

static bool lit(Unread u) {
  return (u.unread && !u.muted) || u.mentions;
}

// what waits in the guilds of `in`, or in all the others when `outside`.
static Unread guilds_unread(App *app, const GuildFolder *in, bool outside) {
  Store *store = app->session->store;
  size_t count = 0;
  const GuildFolder *folders = store_guild_folders(store, &count);
  Unread all = {};

  for (size_t i = 0; i < count; i++) {
    if ((&folders[i] == in) == outside) {
      continue;
    }

    for (size_t j = 0; j < folders[i].nguilds; j++) {
      Unread one = store_guild_unread(store, folders[i].guilds[j]);

      all.unread |= one.unread && !one.muted;
      all.mentions += one.mentions;
    }
  }

  return all;
}

// how long something moving takes, nothing when the system asks for less
// motion.
static float moving(float seconds) {
  return civue_ui_current()->reduce_motion ? 0 : seconds;
}

// discord's pill at the window's edge: tall for the place open, short for
// one with something new, between the two under the pointer.
static void pill(const Look *k, civue_id at, bool open, bool shine,
                 bool hovered) {
  float target = open ? TILE : hovered ? TILE / 2 : shine ? 8 : 0;
  float height = civue_animate(civue_ui_current(), civue_ids(at, "pill"),
                               target, moving(PILL_SECONDS));

  if (height >= .5f) {
    div(absolute, left(px(-(RAIL_WIDTH - TILE) / 2)),
        top(px((TILE - height) / 2)), w(1), h(px(height)), rounded_r(px(4)),
        bg(k->strong)) {}
  }
}

// a mention count at the corner of a tile, cut out of what is under it.
static void corner_badge(const Look *k, uint32_t count, civue_color under) {
  if (count) {
    div(absolute, right(px(-5)), bottom(px(-5)), p(px(2)), rounded(full),
        bg(under)) {
      mention_badge(k, count);
    }
  }
}

// four of the folder's guilds, as discord draws a closed folder.
static void mosaic(App *app, const GuildFolder *f) {
  div(flex, flex_col, gap(px(2))) {
    for (size_t row = 0; row < 2; row++) {
      div(flex, gap(px(2)), h(px(MINI))) {
        for (size_t col = 0; col < 2; col++) {
          size_t at = row * 2 + col;

          div(size(px(MINI)), flex_shrink_0, rounded(md), overflow_hidden) {
            if (at < f->nguilds) {
              guild_icon(app, f->guilds[at], MINI, false);
            }
          }
        }
      }
    }
  }
}

typedef struct {
  civue_id id;
  const char *name;
  // what the tile shows: a guild's icon, a folder's guilds, else `glyph`.
  Guild *guild;
  // no pill at its side, as discord's tile of new servers.
  bool bare;
  const GuildFolder *folder;
  civue_icon glyph;
  // how many points the glyph is drawn at, 20 when 0.
  float glyph_size;
  // a glyph's tile in a folder's color.
  const GuildFolder *tinted;
  // a conversation's tile, its picture round as a person's.
  const Channel *dm;
  // under the name on its card.
  const char *note;
  // what is under the tile, that its badge is cut out of.
  civue_color under;
  // the place open is the tile's, or in it.
  bool open;
  Unread unread;
} Tile;

// a group without a picture, as discord draws it: two of its people, one
// over the other across the tile.
static void facepile(App *app, const Channel *c, bool hovered) {
  const Look *k = &app->look;
  const Store *store = app->session->store;
  constexpr float BOX = TILE * 2 / 3, FACE_OF = BOX / 1.5f;

  div(flex, items_center, justify_center, size(full), rounded(full),
      style(.bg = hovered ? k->primary : CIVUE_CLEAR)) {
    div(relative, size(px(BOX)), flex_shrink_0) {
      for (size_t i = 0; i < 2 && i < c->nrecipients; i++) {
        char url[CDN_URL_MAX];
        float at = i ? BOX - FACE_OF : 0;

        avatar_url(app, store_user(store, c->recipients[i]), 0,
                   (uint32_t)(FACE_OF * 2), url);

        div(absolute, left(px(at)), top(px(at)), size(px(FACE_OF)),
            rounded(full), bg(k->surface_high)) {
          image_fill_at(app, url, FACE_OF / 2, FACE_OF);
        }
      }
    }
  }
}

// a conversation's picture: the person's, the group's, else two of its
// people.
static void dm_art(App *app, const Channel *c, bool hovered) {
  const Look *k = &app->look;
  const Store *store = app->session->store;
  bool group = c->type == CHANNEL_GROUP_DM;
  char url[CDN_URL_MAX] = "";

  if (group) {
    cdn_channel_icon(url, c->id, c->icon, (uint32_t)(TILE * 2));
  }
  else if (c->nrecipients) {
    avatar_url(app, store_user(store, c->recipients[0]), 0,
               (uint32_t)(TILE * 2), url);
  }

  if (group && !*url) {
    facepile(app, c, hovered);
    return;
  }

  div(size(full), rounded(full), overflow_hidden, bg(k->surface_high)) {
    image_fill_at(app, url, TILE / 2, TILE);
  }
}

static void tile_art(App *app, const Tile *t, bool hovered) {
  const Look *k = &app->look;

  if (t->dm) {
    dm_art(app, t->dm, t->open || hovered);
  }
  else if (t->guild) {
    div(flex, items_center, justify_center, size(full), rounded(2xl),
        overflow_hidden) {
      guild_icon(app, t->guild, TILE, t->open || hovered);
    }
  }
  else if (t->folder || t->tinted) {
    civue_color tint = folder_tint(app, t->folder ? t->folder : t->tinted);

    div(flex, items_center, justify_center, size(full), rounded(2xl),
        text_color(k->strong),
        style(.bg = civue_alpha(tint, t->open || hovered ? .32f : .2f))) {
      if (t->folder) {
        mosaic(app, t->folder);
      }
      else {
        icon_of(t->glyph, size(5));
      }
    }
  }
  else {
    bool strong = t->open || hovered;

    div(flex, items_center, justify_center, size(full), rounded(2xl),
        bg(strong ? k->primary : k->surface_high),
        text_color(strong ? k->on_primary : k->strong)) {
      icon_of(t->glyph, size(px(t->glyph_size ? t->glyph_size : 20)));
    }
  }
}

// the tile under the pointer long enough for its card, which is drawn once
// the rail is.
typedef struct {
  civue_id id;
  // where the tile's picture is.
  civue_rect at;
  Snowflake guild_id, folder;
  char name[160];
  const char *note;
} Hover;

// a place of the rail. true when pressed, the tile's rect in `rect`. its
// name is on its card, in `hover` while the pointer rests on it.
static bool tile(App *app, const Tile *t, civue_rect *rect, Hover *hover) {
  civue_ui *ui = civue_ui_current();
  const Look *k = &app->look;
  bool shine = lit(t->unread);
  bool pressed = false;

  div(key(t->id), flex, flex_col, items_center, flex_shrink_0, w(full),
      cursor_pointer) {
    civue_button_state state = button();
    bool hovered = state.response.hovered;
    float sink = civue_animate(ui, civue_ids(t->id, "press"),
                               state.held ? PRESS_INSET : 0,
                               moving(PRESS_SECONDS));

    pressed = state.activated;

    div(relative, flex, items_center, justify_center, size(px(TILE)),
        flex_shrink_0) {
      *rect = here();

      div(flex_shrink_0, size(px(TILE - 2 * sink))) {
        tile_art(app, t, hovered);
      }

      corner_badge(k, t->unread.mentions, t->under);

      if (!t->bare) {
        pill(k, t->id, t->open, shine, hovered);
      }
    }

    if (tooltip(t->id, &state.response)) {
      *hover = (Hover){
        .id = t->id,
        .at = *rect,
        .guild_id = t->guild ? t->guild->id : 0,
        .folder = t->folder ? folder_key(t->folder) : 0,
        .note = t->note,
      };
      snprintf(hover->name, sizeof hover->name, "%s", t->name);
    }
  }

  return pressed;
}

static Tile guild_tile(App *app, Guild *guild, civue_color under) {
  return (Tile){
    .id = id_at(id("rail-guild"), guild->id),
    .name = guild->name ? guild->name : "",
    .guild = guild,
    .under = under,
    .open = app->guild_id == guild->id,
    .unread = store_guild_unread(app->session->store, guild),
  };
}

// the tiles that wait to be read out of a list's view, the nearest each way,
// for a pill at that edge to scroll to them.
typedef struct {
  civue_id list;
  civue_rect view;
  civue_rect up, down;
  bool above, below;
  uint32_t above_mentions, below_mentions;
} Beyond;

static void beyond_see(Beyond *b, civue_rect r, Unread u) {
  double middle = (r.y0 + r.y1) / 2;

  if (!lit(u) || civue_rect_h(r) <= 0 || civue_rect_h(b->view) <= 0) {
    return;
  }

  if (middle < b->view.y0) {
    b->up = !b->above || r.y0 > b->up.y0 ? r : b->up;
    b->above = true;
    b->above_mentions += u.mentions;
  }
  else if (middle > b->view.y1) {
    b->down = !b->below || r.y0 < b->down.y0 ? r : b->down;
    b->below = true;
    b->below_mentions += u.mentions;
  }
}

// brings `r`, a tile of `list`, in view, scrolling there or at once.
static void scroll_to_tile(civue_id list, civue_rect view, civue_rect r,
                           bool snap) {
  civue_ui *ui = civue_ui_current();
  float at = civue_scroll_y(ui, list), height = (float)civue_rect_h(view);
  float y0 = (float)(r.y0 - view.y0) + at - 24;
  float y1 = y0 + (float)civue_rect_h(r) + 60;

  if (snap || ui->reduce_motion) {
    civue_set_scroll_y(ui, list, y0 < at            ? y0
                                 : y1 > at + height ? y1 - height
                                                    : at);
  }
  else {
    civue_scroll_to(ui, list, y0, y1, height);
  }

  civue_request_repaint(ui);
}

// what a pill at a list's edge last said, kept while it fades away.
typedef struct {
  uint32_t mentions;
  civue_rect target;
} Kept;

static bool beyond_body(App *app, civue_id at, bool up, uint32_t mentions,
                        float shown) {
  const Look *k = &app->look;
  civue_color fill = mentions ? k->danger : k->strong;
  civue_color held = mentions ? k->danger_hover : k->text;
  bool pressed = false;
  char count[16];

  snprintf(count, sizeof count, mentions > 99 ? "99+" : "%u", mentions);

  div(key(at), flex, items_center, gap(px(2)), h(5), pl(px(6)), pr(2),
      rounded(full), text_size(px(11)), line_height(px(14)),
      font_weight(bold), tabular_nums, bg(civue_alpha(fill, shown)),
      text_color(civue_alpha(mentions ? k->on_danger : k->frame, shown)),
      style(.shadow = shown > .9f ? CIVUE_SHADOW_MD : CIVUE_SHADOW_NONE),
      hover(bg(civue_alpha(held, shown))), cursor_pointer) {
    civue_button_state state = button();

    pressed = state.activated;

    if (up) {
      icon(mingcute, arrow_up_fill, size(px(11)));
    }
    else {
      icon(mingcute, arrow_down_fill, size(px(11)));
    }

    text(mentions ? count : "New");
    hint(app, at, &state.response, mentions ? "Unread Mentions" : "Unread",
         CIVUE_TOOLTIP_RIGHT);
  }

  return pressed;
}

// a pill at the edge of a list past which something waits, coming in from
// that edge. pressed, the list scrolls to the nearest.
static void beyond_pill(App *app, const Beyond *b, bool up) {
  civue_ui *ui = civue_ui_current();
  civue_id at = civue_ids(b->list, up ? "up" : "down");
  bool on = up ? b->above : b->below;
  Kept *kept = civue_memory(ui, civue_ids(at, "kept"), sizeof *kept, nullptr);
  float shown = civue_animate(ui, civue_ids(at, "shown"), on ? 1 : 0,
                              on ? BEYOND_IN_SECONDS : BEYOND_OUT_SECONDS);
  float edge = ui->reduce_motion ? 6 : 6 - (1 - shown) * 6;
  bool pressed = false;

  if (on) {
    kept->mentions = up ? b->above_mentions : b->below_mentions;
    kept->target = up ? b->up : b->down;
  }

  if (shown < .02f) {
    return;
  }

  if (up) {
    div(absolute, top(px(edge)), left(0), right(0), flex, justify_center) {
      pressed = beyond_body(app, at, up, kept->mentions, shown);
    }
  }
  else {
    div(absolute, bottom(px(edge)), left(0), right(0), flex,
        justify_center) {
      pressed = beyond_body(app, at, up, kept->mentions, shown);
    }
  }

  if (pressed && on) {
    scroll_to_tile(b->list, b->view, kept->target, false);
  }
}

// the edges of a list where it hides more, faded into the rail along an
// eased ramp so that no edge shows. `end` is where its tiles end.
static void edge_fades(App *app, const Beyond *b, civue_rect end) {
  civue_color frame = app->look.frame;
  bool above = civue_scroll_y(civue_ui_current(), b->list) > .5f;
  bool below = civue_rect_h(b->view) > 0 && end.y0 > b->view.y1 + .5;

  for (int side = 0; side < 2; side++) {
    if (!(side ? below : above)) {
      continue;
    }

    // the bottom's ramp runs up from the edge.
    div(absolute, left(0), right(0), h(px(FADE)), flex,
        style(.direction = side ? CIVUE_COL_REVERSE : CIVUE_COL,
              .inset[0] = side ? (civue_length){} : civue_px(0),
              .inset[2] = side ? civue_px(0) : (civue_length){})) {
      for (int i = 0; i < FADE; i++) {
        float t = (i + .5f) / FADE;

        div(flex_shrink_0, h(px(1)),
            bg(civue_alpha(frame, 1 - t * t * (3 - 2 * t)))) {}
      }
    }
  }
}

static void beyond_pills(App *app, const Beyond *b) {
  beyond_pill(app, b, true);
  beyond_pill(app, b, false);
}

// brings the tile of a guild opened elsewhere in view, at once.
static void reveal(App *app, civue_id list, civue_rect view, civue_rect r) {
  if (app->rail_reveal && civue_rect_h(r) > 0 && civue_rect_h(view) > 0) {
    scroll_to_tile(list, view, r, true);
  }
}

// what a press of the rail asks, done once it is drawn.
typedef struct {
  Guild *guild;
  const Channel *dm;
  Snowflake folder;
  bool back, messages, explore, create;
  Hover hover;
} Press;

// a conversation under the messages, and how much of it shows as it comes
// or goes.
typedef struct {
  const Channel *channel;
  float shown;
} Dm;

// the conversations the rail showed, the ones leaving with them.
typedef struct {
  size_t count;
  Snowflake ids[DMS];
  bool ready;
} Dms;

static bool listed(const Snowflake *ids, size_t count, Snowflake id) {
  for (size_t i = 0; i < count; i++) {
    if (ids[i] == id) {
      return true;
    }
  }

  return false;
}

// a conversation with something new: any message, or a mention in one
// muted.
static bool waits(App *app, const Channel *c) {
  return lit(store_channel_unread(app->session->store, c));
}

// the conversations with something new, the latest first, and the ones
// just read leaving from where they were. what the rail showed at launch
// is there at once.
static size_t dms_of(App *app, Dm *out) {
  civue_ui *ui = civue_ui_current();
  Store *store = app->session->store;
  Dms *kept = civue_memory(ui, id("rail-dms"), sizeof *kept, nullptr);
  size_t count = 0, n = 0, shown = 0;
  Channel *const *channels = store_private_channels(store, &count);
  const Channel *call = store_channel(store, app->session->voice_channel_id);
  Snowflake order[DMS];

  // the call the account is in comes first, even read.
  if (call && channel_is_private(call)) {
    order[n++] = call->id;
  }

  for (size_t i = 0; i < count && n < DMS; i++) {
    if (channels[i] != call && waits(app, channels[i])) {
      order[n++] = channels[i]->id;
    }
  }

  for (size_t i = 0; i < kept->count && n < DMS; i++) {
    size_t at = i < n ? i : n;

    if (listed(order, n, kept->ids[i])) {
      continue;
    }

    memmove(order + at + 1, order + at, (n - at) * sizeof *order);
    order[at] = kept->ids[i];
    n++;
  }

  kept->count = 0;

  for (size_t i = 0; i < n; i++) {
    const Channel *c = store_channel(store, order[i]);
    bool on = c && (c == call || waits(app, c));
    // nothing moves where the user is not looking.
    float seconds = ui->window_focused
                      ? moving(on ? DM_IN_SECONDS : DM_OUT_SECONDS)
                      : 0;
    float seen = civue_animate_from(ui, id_at(id("rail-dm"), order[i]),
                                    kept->ready ? 0 : 1, on, seconds);

    if (!c || (!on && seen < .01f)) {
      continue;
    }

    kept->ids[kept->count++] = order[i];
    out[shown++] = (Dm){c, seen};
  }

  kept->ready = true;
  return shown;
}

// the requests of friends waiting, on the messages as discord counts them.
static Unread requests(App *app) {
  const Store *store = app->session->store;
  Unread u = {};

  for (size_t i = 0; i < store->nrelationships; i++) {
    u.mentions += store->relationships[i].type == RELATIONSHIP_INCOMING;
  }

  return u;
}

// what waits in the conversations.
static Unread dms_unread(App *app) {
  Unread u = store_private_unread(app->session->store);

  return (Unread){.unread = u.unread && !u.muted, .mentions = u.mentions};
}

// a conversation's tile, in a place that grows with it as it comes and
// shrinks as it goes, the tiles under it moving along.
static void dm_tile(App *app, const Dm *dm, Press *press, Beyond *b) {
  const Look *k = &app->look;
  const Channel *c = dm->channel;
  Unread unread = store_channel_unread(app->session->store, c);
  float s = dm->shown;
  civue_rect r = {};
  char name[160];

  store_channel_name(app->session->store, c, name, sizeof name);

  Tile t = {
    .id = id_at(id("rail-dm-tile"), c->id),
    .name = name,
    .dm = c,
    .under = k->frame,
    .open = app->session->selected_channel_id == c->id,
    .unread = unread,
  };

  div(relative, flex_shrink_0, w(full), h(px((TILE + GAP) * s))) {
    civue_rect slot = here();
    // where the tile is as big as it shows, in the middle of its room.
    float middle = GAP * s + TILE * s / 2;
    civue_vec2 pivot = {(slot.x0 + slot.x1) / 2, slot.y0 + middle};

    div(absolute, left(0), right(0), top(px(middle - TILE / 2))) {
      faded(s) {
        zoomed(DM_FROM + (1 - DM_FROM) * s, pivot) {
          if (tile(app, &t, &r, &press->hover) && s > .5f) {
            press->dm = c;
          }
        }
      }
    }
  }

  if (s > .99f) {
    beyond_see(b, r, unread);
  }
}

// the messages, the conversations waiting, the servers and the folders in
// the user's order, then what adds more of them.
static void top_list(App *app, const Dm *dms, size_t ndms, Press *press) {
  const Look *k = &app->look;
  size_t count = 0;
  const GuildFolder *folders = store_guild_folders(app->session->store, &count);
  Beyond b = {.list = id("rail-top")};
  civue_rect r = {}, end = {};
  Tile messages = {
    .id = id("rail-messages"),
    .name = "Direct Messages",
    .glyph = CIVUE_ICON(ph, chat_circle_fill),
    // the bubble as big as half the tile, the icon's own margin around it.
    .glyph_size = 30,
    .under = k->frame,
    .open = !app->guild_id && !app->exploring,
    .unread = requests(app),
  };

  div(key(b.list), flex, flex_col, items_center, gap(2), h(full), pt(1),
      pb(3), overflow_y_scroll, scrollbars(&app->thin_bars)) {
    b.view = here();

    div(flex, flex_col, items_center, flex_shrink_0, w(full)) {
      press->messages = tile(app, &messages, &r, &press->hover);

      if (messages.open && !app->rail_folder) {
        reveal(app, b.list, b.view, r);
      }

      for (size_t i = 0; i < ndms; i++) {
        dm_tile(app, &dms[i], press, &b);
      }
    }

    div(flex_shrink_0, w(px(32)), h(px(1)), rounded(px(1)), bg(k->border)) {}

    for (size_t i = 0; i < count; i++) {
      const GuildFolder *f = &folders[i];
      Tile t = {};
      char name[160];

      if (!f->nguilds) {
        continue;
      }

      if (is_folder(f)) {
        folder_name(f, name, sizeof name);
        t = (Tile){
          .id = id_at(id("rail-folder"), folder_key(f)),
          .name = name,
          .folder = f,
          .under = k->frame,
          .open = folder_of(app, app->guild_id) == f,
          .unread = guilds_unread(app, f, false),
        };
      }
      else {
        t = guild_tile(app, f->guilds[0], k->frame);
      }

      if (tile(app, &t, &r, &press->hover)) {
        press->guild = t.guild;
        press->folder = t.folder ? folder_key(f) : 0;
      }

      beyond_see(&b, r, t.unread);

      if (t.guild && t.open && !app->rail_folder) {
        reveal(app, b.list, b.view, r);
      }
    }

    Tile create = {
      .id = id("rail-create"),
      .name = "Add a Server",
      .glyph = CIVUE_ICON(mingcute, add_fill),
      .bare = true,
      .under = k->frame,
      .open = app->popover == POPOVER_NEW_GUILD,
    };
    Tile explore = {
      .id = id("rail-explore"),
      .name = "Discover",
      .glyph = CIVUE_ICON(mingcute, compass_fill),
      .under = k->frame,
      .open = app->exploring,
    };

    press->create = tile(app, &create, &r, &press->hover);
    press->explore = tile(app, &explore, &r, &press->hover);

    div(flex_shrink_0, h(0)) {
      end = here();
    }
  }

  edge_fades(app, &b, end);
  beyond_pills(app, &b);
}

// the folder the rail is in: its tile, pinned over its guilds, leads back
// and carries what waits outside it, as ios' back button does.
static void folder_list(App *app, const GuildFolder *f, Press *press) {
  const Look *k = &app->look;
  Beyond b = {.list = id_at(id("rail-in"), folder_key(f))};
  civue_rect r = {}, end = {};
  char name[160];

  folder_name(f, name, sizeof name);

  Tile head = {
    .id = id("rail-back"),
    .name = name,
    .glyph = CIVUE_ICON(mingcute, arrow_left_fill),
    .tinted = f,
    .note = "Back to Servers",
    .under = k->frame,
    .unread = guilds_unread(app, f, true),
  };
  Unread dms = dms_unread(app);

  // the conversations are out of the folder too.
  head.unread.unread |= dms.unread;
  head.unread.mentions += dms.mentions;

  div(flex, flex_col, items_center, h(full)) {
    div(flex_shrink_0, w(full), pt(3), pb(3)) {
      press->back = tile(app, &head, &r, &press->hover);
    }

    div(flex_shrink_0, w(px(24)), h(px(1)), bg(k->border)) {}

    div(relative, flex_1, min_h(0), w(full)) {
      div(key(b.list), flex, flex_col, items_center, gap(2), h(full), pt(3),
          pb(3), overflow_y_scroll, scrollbars(&app->thin_bars)) {
        b.view = here();

        for (size_t i = 0; i < f->nguilds; i++) {
          Tile t = guild_tile(app, f->guilds[i], k->frame);

          if (tile(app, &t, &r, &press->hover)) {
            press->guild = t.guild;
          }

          beyond_see(&b, r, t.unread);

          if (t.open && app->rail_folder == folder_key(f)) {
            reveal(app, b.list, b.view, r);
          }
        }

        div(flex_shrink_0, h(0)) {
          end = here();
        }
      }

      edge_fades(app, &b, end);
      beyond_pills(app, &b);
    }
  }
}

// a guild opened elsewhere, from a mention or the launcher, brings the rail
// to its tile, in its folder, and the conversations bring it to the
// messages. what the user did not do on the rail does not move on it: it
// snaps there.
static void follow(App *app) {
  civue_ui *ui = civue_ui_current();

  if (app->rail_reveal && !ui->rerun) {
    app->rail_reveal--;
  }

  // the folder was undone, or its last guild left.
  if (app->rail_folder && !find_folder(app, app->rail_folder)) {
    app->rail_folder = 0;
  }

  if (app->guild_id == app->rail_guild) {
    return;
  }

  app->rail_guild = app->guild_id;

  const GuildFolder *f = folder_of(app, app->guild_id);

  app->rail_folder = f ? folder_key(f) : 0;
  app->rail_shown = f ? app->rail_folder : app->rail_shown;
  app->rail_reveal = REVEAL;
  app->rail_snaps++;
}

static void apply(App *app, const Press *press) {
  if (press->guild || press->dm || press->messages) {
    if (app->popover == POPOVER_GUILD_MENU || app->popover == POPOVER_INVITE) {
      app->popover = POPOVER_NONE;
    }
  }

  if (press->guild) {
    app_select_guild(app, press->guild->id);
  }
  else if (press->dm) {
    app_select_channel(app, press->dm->id);
  }
  else if (press->folder) {
    app->rail_folder = press->folder;
    app->rail_shown = press->folder;
  }
  else if (press->back) {
    app->rail_folder = 0;
  }
  // as discord's home, the messages open the last conversation.
  else if (press->messages && (app->guild_id || app->exploring)) {
    app_select_guild(app, 0);
  }
  else if (press->explore) {
    app->popover = POPOVER_NONE;
    explore_open(app);
  }
  else if (press->create) {
    app_toggle(app, POPOVER_NEW_GUILD);
  }

  // pressed in view, the rail stays as it is.
  app->rail_guild = app->guild_id;
}

// the mark before a guild's name on its card, as discord's.
static void card_badge(App *app, const Guild *g) {
  const Look *k = &app->look;
  civue_color fill = k->border_strong, on = k->strong;
  civue_icon glyph_icon = CIVUE_ICON(mingcute, earth_fill);

  if (store_guild_has(g, "VERIFIED")) {
    fill = k->positive, on = k->on_positive;
    glyph_icon = CIVUE_ICON(mingcute, check_fill);
  }
  else if (store_guild_has(g, "PARTNERED")) {
    fill = k->primary, on = k->on_primary;
    glyph_icon = CIVUE_ICON(mingcute, check_fill);
  }
  else if (g->hub) {
    glyph_icon = CIVUE_ICON(mingcute, mortarboard_fill);
  }
  else if (!g->community) {
    return;
  }

  div(flex, items_center, justify_center, flex_shrink_0, size(4), mt(px(2)),
      rounded(full), bg(fill)) {
    glyph(glyph_icon, 11, on);
  }
}

// who of a guild is in its voice channels, or sharing their screen there.
// their number, the first `cap` in `out`.
static size_t in_voice(App *app, Snowflake guild_id, bool streaming,
                       const User **out, size_t cap) {
  const Store *store = app->session->store;
  size_t n = 0;

  for (size_t i = 0; i < store->nvoice_states; i++) {
    const VoiceState *v = &store->voice_states[i];

    if (v->guild_id != guild_id || !v->channel_id ||
        (streaming && !v->streaming)) {
      continue;
    }

    if (n < cap) {
      out[n] = store_user(store, v->user_id);
    }

    n++;
  }

  return n;
}

// a face of the stack, cut out of the one before it by a ring of the card.
static void face(App *app, const User *user, Snowflake guild_id, bool first) {
  const Look *k = &app->look;
  char url[CDN_URL_MAX];

  avatar_url(app, user, guild_id, (uint32_t)(FACE * 2), url);

  div(flex_shrink_0, size(px(FACE + 4)), p(px(2)), rounded(full),
      bg(k->popover), ml(px(first ? 0 : -8))) {
    div(size(full), rounded(full), bg(k->surface_high)) {
      image_fill_at(app, url, FACE / 2, FACE);
    }
  }
}

// a row of who is in voice, or streaming, as discord's card puts them.
static void faces(App *app, civue_icon glyph_icon, Snowflake guild_id,
                  bool streaming) {
  const Look *k = &app->look;
  const User *users[FACES] = {};
  size_t count = in_voice(app, guild_id, streaming, users, FACES);
  char more[24];

  if (!count) {
    return;
  }

  snprintf(more, sizeof more, "+%zu", count - FACES);

  div(flex, items_center, gap(2)) {
    glyph(glyph_icon, 16, k->subtle);

    div(flex, items_center) {
      for (size_t i = 0; i < count && i < FACES; i++) {
        face(app, users[i], guild_id, i == 0);
      }

      if (count > FACES) {
        div(flex, items_center, justify_center, flex_shrink_0,
            h(px(FACE + 4)), min_w(px(FACE + 4)), p(px(2)), ml(px(-8)),
            rounded(full), bg(k->popover)) {
          div(flex, items_center, justify_center, h(full), min_w(full),
              px(px(6)), rounded(full), bg(k->surface_high)) {
            text(more, text_size(px(12)), line_height(px(16)),
                 font_weight(bold), tabular_nums, text_color(k->text));
          }
        }
      }
    }
  }
}

static void card_body(App *app, const Hover *h) {
  const Look *k = &app->look;
  Guild *guild =
    h->guild_id ? store_guild(app->session->store, h->guild_id) : nullptr;
  const GuildFolder *f = h->folder ? find_folder(app, h->folder) : nullptr;
  const char *note = h->note;
  char servers[32];

  if (f) {
    snprintf(servers, sizeof servers,
             f->nguilds == 1 ? "%zu server" : "%zu servers", f->nguilds);
    note = servers;
  }

  div(flex, items_start, gap(2)) {
    if (guild) {
      card_badge(app, guild);
    }

    text(h->name, min_w(0), text_size(px(16)), line_height(px(20)),
         font_weight(semibold), text_color(k->strong));
  }

  if (note) {
    text(note, mt(px(-6)), text_size(px(13)), line_height(px(18)),
         text_color(k->subtle));
  }

  if (guild) {
    faces(app, CIVUE_ICON(mingcute, volume_fill), guild->id, false);
    faces(app, CIVUE_ICON(mingcute, monitor_fill), guild->id, true);

    if (guild->muted) {
      text("Muted", text_size(px(13)), line_height(px(18)),
           text_color(k->faint));
    }
  }
}

// how far the card's arrow reaches out of it.
constexpr double ARROW = 7;

// where the arrow of a card at `top`, `height` tall, points to `y`: by its
// tile, and never past its corners.
static double arrow_y(double top, double height, double y) {
  return fmin(fmax(y, top + 12), fmax(top + height - 12, top + 12));
}

// an arrow out of the card's left side at `y`, its border around it. drawn
// as thin strips, civue draws no triangle.
static void card_arrow(const Look *k, civue_rect card, double y) {
  constexpr int STRIPS = (int)(ARROW * 2);

  // the inside is the outside moved in by the border's width across its
  // slant, so it covers the card's border where they meet.
  for (int pass = 0; pass < 2; pass++) {
    double from = card.x0 + (pass ? 1.5 : 0);
    civue_color color = pass ? k->popover : k->border_strong;

    for (int i = 0; i < STRIPS; i++) {
      double half = ARROW - i * .5;

      fill(xywh(from - (i + 1) * .5, y - half, .5, half * 2), color, 0);
    }
  }
}

// the card of the tile under the pointer, as discord's: the place's name and
// who is in voice there. it comes from its tile and moves at once from one
// tile to the next, waiting over the gap between them.
static void rail_card(App *app, const Hover *hover, civue_rect rail) {
  civue_ui *ui = civue_ui_current();
  const Look *k = &app->look;
  civue_id at = id("rail-card");
  Hover *last = civue_memory(ui, civue_ids(at, "last"), sizeof *last, nullptr);
  double *seen = civue_memory(ui, civue_ids(at, "seen"), sizeof *seen, nullptr);
  double since = ui->now - *seen;
  bool between = last->id && since < CARD_GAP_SECONDS &&
                 ui->input.has_mouse &&
                 civue_rect_contains(rail, ui->input.mouse);
  bool on = hover->id != 0;

  if (on) {
    *last = *hover;
    *seen = ui->now;
  }
  else if (between) {
    civue_request_repaint_after(ui, CARD_GAP_SECONDS - since);
  }

  float shown = civue_animate(ui, civue_ids(at, "shown"), on || between,
                              on ? CARD_IN_SECONDS : CARD_OUT_SECONDS);

  if (shown < .01f || !last->id) {
    return;
  }

  civue_vec2 size = last_size(at);
  civue_tooltip_place place =
    tooltip_at(last->at, size, CIVUE_TOOLTIP_RIGHT, 14);
  float y = (float)fmin(fmax(place.rect.y0, 8),
                        fmax(ui->size.y - size.y - 8, 8));
  double arrow = arrow_y(y, size.y, (last->at.y0 + last->at.y1) / 2);
  float grown =
    ui->reduce_motion ? 1 : CARD_FROM + (1 - CARD_FROM) * shown;
  civue_vec2 tip = {place.rect.x0 - ARROW, arrow};
  civue_rect card = {};

  overlay() {
    // unmeasured, it waits for the pass that knows its size.
    faded(size.y > 0 ? shown : 0) {
      zoomed(grown, tip) {
        div(key(at), flex, flex_col, gap(2), left(px(place.rect.x0)),
            top(px(y)), max_w(px(CARD_WIDTH)), px(3), py(px(10)),
            rounded(lg), bg(k->popover), border(1),
            border_color(k->border_strong), shadow(lg)) {
          card = here();
          card_body(app, last);
        }

        card_arrow(k, card, arrow);
      }
    }
  }
}

void server_rail(App *app) {
  civue_ui *ui = civue_ui_current();
  Press press = {};
  civue_rect rail = {};
  Dm dms[DMS];
  size_t ndms = dms_of(app, dms);

  follow(app);

  const GuildFolder *shown = find_folder(app, app->rail_shown);
  bool in = app->rail_folder && shown;
  float slide =
    civue_animate(ui, id_at(id("rail-slide"), app->rail_snaps), in ? 1 : 0,
                  moving(in ? SLIDE_IN_SECONDS : SLIDE_OUT_SECONDS));

  // the top and the folder side by side, the rail sliding from one to the
  // other.
  div(key(id("rail")), relative, flex_shrink_0, w(px(RAIL_WIDTH)), h(full),
      overflow_hidden) {
    rail = here();

    div(absolute, top(0), left(0), w(full), h(px(RAIL_TOP))) {
      window_handle(id("title-bar"));
    }

    if (slide < 1 || !in) {
      div(absolute, top(px(RAIL_TOP)), bottom(0), w(full),
          left(px(-slide * RAIL_WIDTH))) {
        top_list(app, dms, ndms, &press);
      }
    }

    if (shown && (slide > 0 || in)) {
      div(absolute, top(px(RAIL_TOP)), bottom(0), w(full),
          left(px((1 - slide) * RAIL_WIDTH))) {
        folder_list(app, shown, &press);
      }
    }
  }

  rail_card(app, &press.hover, rail);
  new_guild(app);
  apply(app, &press);
}
