#include "widgets.h"

#include <math.h>
#include <stdio.h>
#include <time.h>

#include <civue/dsl.h>

#include "discord/emoji.h"
#include "discord/cdn.h"

void glyph(civue_icon icon, float size, civue_color color) {
  civue_node_icon(civue_ui_current(), icon,
                  &CIVUE_NODE_STYLE(.w = civue_px(size), .h = civue_px(size),
                                    .shrink = 0, .has_shrink = true,
                                    .text_color = color,
                                    .has_text_color = true));
}

bool image_fill(App *app, const char *url, float radius) {
  civue_ui *ui = civue_ui_current();
  civue_rect r = here();
  double side = fmax(civue_rect_w(r), civue_rect_h(r));

  if (side <= 0) {
    return false;
  }

  civue_image_info image =
    civue_images_get(app->images, ui, url, (uint32_t)(side * ui->scale));

  if (image.state != CIVUE_IMAGE_READY) {
    return false;
  }

  civue_image(ui, image.texture, r, (float[4]){0, 0, 1, 1}, 1, radius, false);
  return true;
}

bool sticker_fill(App *app, Snowflake sticker_id, uint8_t format, bool animated,
                  float opacity) {
  civue_ui *ui = civue_ui_current();
  civue_rect r = here();
  double side = fmax(civue_rect_w(r), civue_rect_h(r));
  // the client asks for twice the points at most.
  uint32_t asked = (uint32_t)ceil(side * fmin(ui->scale, 2));
  uint32_t pixels = (uint32_t)ceil(side * ui->scale);
  bool moves = format == STICKER_APNG || format == STICKER_GIF;
  civue_image_info image = {};
  char url[CDN_URL_MAX];

  if (side <= 0) {
    return false;
  }

  if (animated && moves) {
    cdn_sticker(url, sticker_id, (StickerFormat)format, asked, true);
    image = civue_images_get_animated(app->images, ui, url, pixels);
  }

  if (image.state != CIVUE_IMAGE_READY) {
    cdn_sticker(url, sticker_id, (StickerFormat)format, asked, false);
    image = civue_images_get(app->images, ui, url, pixels);
  }

  if (image.state != CIVUE_IMAGE_READY || !image.width || !image.height) {
    return false;
  }

  // inside the node, as css's contain.
  double w = civue_rect_w(r), h = civue_rect_h(r);
  double fit = fmin(w / image.width, h / image.height);
  double iw = image.width * fit, ih = image.height * fit;
  civue_rect at = xywh(r.x0 + (w - iw) / 2, r.y0 + (h - ih) / 2, iw, ih);

  civue_image(ui, image.texture, at, (float[4]){0, 0, 1, 1}, opacity, 0, false);
  return true;
}

void emoji_node(App *app, Snowflake emoji_id, const char *name, float size) {
  const UnicodeEmoji *emoji = emoji_id ? nullptr : emoji_by_utf8(name);
  char url[CDN_URL_MAX] = "";

  if (emoji_id) {
    cdn_emoji(url, emoji_id, 48);
  }
  else if (emoji) {
    emoji_url(url, emoji);
  }

  if (!*url) {
    text(name ? name : "", flex_shrink_0, text_size(px(size * 0.85f)),
         line_height(px(size)));
    return;
  }

  div(size(px(size)), flex, items_center, justify_center, flex_shrink_0) {
    civue_ui *ui = civue_ui_current();
    civue_rect r = here();
    uint32_t pixels =
      (uint32_t)(fmax(civue_rect_w(r), civue_rect_h(r)) * ui->scale);

    // an emoji of unicode whose image discord no longer has is the
    // system's glyph, as one discord never had.
    if (emoji && pixels &&
        civue_images_get(app->images, ui, url, pixels).state ==
          CIVUE_IMAGE_FAILED) {
      text(name, text_size(px(size * 0.85f)), line_height(px(size)));
    }
    else {
      image_fill(app, url, 0);
    }
  }
}

void status_dot(UserStatus status, civue_rect r, const Look *k,
                civue_color ring) {
  double size = civue_rect_w(r);
  civue_color color = k->offline;

  switch (status) {
    case STATUS_ONLINE:
      color = k->online;
      break;
    case STATUS_IDLE:
      color = k->idle;
      break;
    case STATUS_DND:
      color = k->dnd;
      break;
    default:
      break;
  }

  fill(civue_rect_inflate(r, 3, 3), ring, (float)size);
  fill(r, color, (float)size);

  // the shapes discord cuts in the dot: a ring, a moon and a bar.
  if (status == STATUS_OFFLINE) {
    fill(civue_rect_inflate(r, -size * .25, -size * .25), ring, (float)size);
  }
  else if (status == STATUS_IDLE) {
    fill(xywh(r.x0 - size * .1, r.y0 - size * .1, size * .6, size * .6), ring,
         (float)size);
  }
  else if (status == STATUS_DND) {
    fill(civue_rect_inflate(r, -size * .2, -size * .38), ring, 2);
  }
}

void avatar(App *app, const User *user, Snowflake guild_id, float size,
            bool status, civue_color ring) {
  char url[CDN_URL_MAX] = "", frame[CDN_URL_MAX] = "";
  uint32_t pixels = (uint32_t)(size * 2);

  if (user) {
    const Guild *guild =
      guild_id ? store_guild(app->session->store, guild_id) : nullptr;
    const Member *member = guild ? store_member(guild, user->id) : nullptr;
    const char *worn =
      member && member->decoration ? member->decoration : user->decoration;

    // discord draws it a fifth wider than the avatar it frames.
    cdn_avatar_decoration(frame, worn, (uint32_t)(size * 1.2f * 2));

    if (member && member->avatar) {
      cdn_member_avatar(url, guild_id, user->id, member->avatar, pixels);
    }
    else {
      cdn_avatar(url, user->id, user->avatar, user->discriminator, pixels);
    }
  }

  div(w(px(size)), h(px(size)), flex_shrink_0, rounded(full),
      bg(app->look.surface_high)) {
    image_fill(app, url, size / 2);

    if (*frame) {
      div(absolute, left(px(-size * .1f)), top(px(-size * .1f)),
          size(px(size * 1.2f))) {
        image_fill(app, frame, 0);
      }
    }

    if (status && user) {
      civue_rect r = here();
      double dot = size >= 40 ? 12 : 10;

      const Store *store = app->session->store;
      UserStatus shown =
        user->id == store->self_id ? store->self_status : user->status;

      status_dot(shown, xywh(r.x1 - dot, r.y1 - dot, dot, dot), &app->look,
                 ring);
    }
  }
}

void tag_chip(App *app, const User *u) {
  const Look *k = &app->look;
  char url[CDN_URL_MAX];

  if (!u->tag || !*u->tag) {
    return;
  }

  cdn_tag_badge(url, u->tag_guild_id, u->tag_badge, 32);

  div(flex, items_center, gap(1), flex_shrink_0, h(px(18)), px(1), rounded(sm),
      bg(k->hover)) {
    if (*url) {
      div(size(px(12)), flex_shrink_0) {
        image_fill(app, url, 0);
      }
    }

    text(u->tag, text_size(px(12)), line_height(px(16)), font_weight(semibold),
         text_color(k->text), whitespace_nowrap);
  }
}

void mention_badge(const Look *k, uint32_t count) {
  if (!count) {
    return;
  }

  char label[16];
  snprintf(label, sizeof label, count > 999 ? "999+" : "%u", count);

  div(flex, items_center, justify_center, flex_shrink_0, h(4), min_w(4), px(1),
      rounded(full), bg(k->danger)) {
    text(label, text_size(px(12)), line_height(px(16)), font_weight(bold),
         text_color(CIVUE_WHITE));
  }
}

civue_rect overlay_room(void) {
  civue_ui *ui = civue_ui_current();
  civue_rect s = ui->screen;

  ui->overlay_outside = true;
  return (civue_rect){s.x0 + 8, s.y0 + 8, s.x1 - 8, s.y1 - 8};
}

void hint(App *app, civue_id id, const civue_response *resp, const char *label,
          civue_tooltip_side side) {
  if (!tooltip(id, resp)) {
    return;
  }

  const Look *k = &app->look;
  civue_id tip = id_in(id, "tip");
  civue_tooltip_place at = tooltip_at(resp->rect, last_size(tip), side, 8);

  overlay() {
    div(key(tip), left(px(at.rect.x0)), top(px(at.rect.y0)), px(3), py(2),
        rounded(lg), bg(k->surface_high), border(1),
        border_color(k->border_strong), shadow(lg), whitespace_nowrap,
        text_size(px(14)), line_height(px(18)), font_weight(semibold),
        text_color(k->text)) {
      text(label);
    }
  }
}

bool icon_button(App *app, civue_id id, civue_icon icon, float size,
                 float icon_size, const char *label) {
  const Look *k = &app->look;
  bool pressed = false;

  div(key(id), flex, items_center, justify_center, flex_shrink_0, w(px(size)),
      h(px(size)), rounded(lg), text_color(k->subtle),
      hover(bg(k->hover), text_color(k->strong)), active(bg(k->pressed)),
      cursor_pointer) {
    civue_button_state state = button();
    pressed = state.activated;

    civue_node_icon(
      civue_ui_current(), icon,
      &CIVUE_NODE_STYLE(.w = civue_px(icon_size), .h = civue_px(icon_size)));

    if (label) {
      hint(app, id, &state.response, label, CIVUE_TOOLTIP_ABOVE);
    }
  }

  return pressed;
}

civue_field_look field_look(const Look *k) {
  civue_field_box idle = {
    .bg = rgba(0x0000001f),
    .border = k->border_strong,
    .border_width = 1,
    .radius = 8,
    .text = k->text,
  };

  civue_field_box editing = idle;
  editing.border = k->brand;

  return (civue_field_look){
    .pad = 12,
    .editing = editing,
    .idle = idle,
    .hovered = idle,
    .placeholder = rgb(0x8f9097),
    .selection = civue_alpha(k->brand, .45f),
  };
}

static void paint_bar(const civue_scrollbar *bar, civue_color thumb,
                      float show) {
  if (!bar->shown) {
    return;
  }

  fill(bar->thumb, civue_alpha(thumb, show * (bar->lit ? 1 : .7f)), 4);
}

static void paint_bars([[maybe_unused]] civue_ui *ui, void *ctx,
                       const civue_scroll_state *s) {
  const App *app = ctx;

  paint_bar(&s->vertical, app->look.thumb, s->show);
  paint_bar(&s->horizontal, app->look.thumb, s->show);
}

civue_scrollbar_look scrollbars_of(App *app, float size) {
  return (civue_scrollbar_look){
    .size = size,
    .inset = 2,
    .min_thumb = 32,
    .linger = 1,
    .fade = .2f,
    .ctx = app,
    .paint = paint_bars,
  };
}

static struct tm local_of(int64_t ms) {
  time_t seconds = (time_t)(ms / 1000);
  struct tm tm = {};

  localtime_r(&seconds, &tm);
  return tm;
}

int64_t day_of(int64_t ms) {
  struct tm tm = local_of(ms);

  return (int64_t)(tm.tm_year + 1900) * 1000 + tm.tm_yday;
}

void format_clock(int64_t ms, char *out, size_t cap) {
  struct tm tm = local_of(ms);

  snprintf(out, cap, "%02d:%02d", tm.tm_hour, tm.tm_min);
}

void format_stamp(int64_t ms, char *out, size_t cap) {
  struct tm tm = local_of(ms);
  int64_t now = (int64_t)time(nullptr) * 1000;
  int64_t today = day_of(now);
  int64_t day = day_of(ms);

  if (day == today) {
    snprintf(out, cap, "%02d:%02d", tm.tm_hour, tm.tm_min);
  }
  else if (day == day_of(now - 86400000)) {
    snprintf(out, cap, "Yesterday at %02d:%02d", tm.tm_hour, tm.tm_min);
  }
  else {
    snprintf(out, cap, "%02d/%02d/%04d, %02d:%02d", tm.tm_mday, tm.tm_mon + 1,
             tm.tm_year + 1900, tm.tm_hour, tm.tm_min);
  }
}

void format_day(int64_t ms, char *out, size_t cap) {
  static const char *const months[] = {
    "January", "February", "March",     "April",   "May",      "June",
    "July",    "August",   "September", "October", "November", "December"};
  struct tm tm = local_of(ms);

  snprintf(out, cap, "%d %s %d", tm.tm_mday, months[tm.tm_mon % 12],
           tm.tm_year + 1900);
}

void format_short_day(int64_t ms, char *out, size_t cap) {
  static const char *const months[] = {"Jan", "Feb", "Mar", "Apr",
                                       "May", "Jun", "Jul", "Aug",
                                       "Sep", "Oct", "Nov", "Dec"};
  struct tm tm = local_of(ms);

  snprintf(out, cap, "%s %d, %d", months[tm.tm_mon % 12], tm.tm_mday,
           tm.tm_year + 1900);
}

civue_icon activity_icon(ActivityType type) {
  switch (type) {
    case ACTIVITY_LISTENING:
      return CIVUE_ICON(lucide, music);
    case ACTIVITY_WATCHING:
      return CIVUE_ICON(lucide, tv);
    case ACTIVITY_STREAMING:
      return CIVUE_ICON(lucide, radio);
    case ACTIVITY_COMPETING:
      return CIVUE_ICON(lucide, trophy);
    default:
      return CIVUE_ICON(lucide, gamepad_2);
  }
}

uint32_t mentions_of(Store *store) {
  size_t count = 0;
  const GuildFolder *folders = store_guild_folders(store, &count);
  uint32_t total = store_private_unread(store).mentions;

  for (size_t i = 0; i < count; i++) {
    for (size_t j = 0; j < folders[i].nguilds; j++) {
      total += store_guild_unread(store, folders[i].guilds[j]).mentions;
    }
  }

  return total;
}

bool image_cover(App *app, const char *url, float radius) {
  civue_ui *ui = civue_ui_current();
  civue_rect r = here();
  double side = fmax(civue_rect_w(r), civue_rect_h(r));

  if (side <= 0 || civue_rect_w(r) <= 0) {
    return false;
  }

  civue_image_info image =
    civue_images_get(app->images, ui, url, (uint32_t)(side * ui->scale));

  if (image.state != CIVUE_IMAGE_READY || !image.height) {
    return false;
  }

  float shape = (float)(civue_rect_h(r) / civue_rect_w(r));
  float kept = shape * (float)image.width / (float)image.height;
  float cut_y = kept < 1 ? (1 - kept) / 2 : 0;
  float cut_x = kept > 1 ? (1 - 1 / kept) / 2 : 0;

  civue_image(ui, image.texture, r,
              (float[4]){cut_x, cut_y, 1 - cut_x, 1 - cut_y}, 1, radius, false);
  return true;
}

bool menu_row(App *app, civue_id id, civue_icon icon, const char *label,
              civue_color color) {
  const Look *k = &app->look;
  bool pressed = false;

  div(key(id), flex, items_center, gap(3), flex_shrink_0, h(10), px(2),
      rounded(lg), text_color(color), hover(bg(k->hover)), text_size(px(14)),
      line_height(px(18)), font_weight(medium), cursor_pointer) {
    pressed = button().activated;

    div(flex, items_center, justify_center, size(8), flex_shrink_0) {
      glyph(icon, 18, color);
    }

    text(label, flex_1, min_w(0), truncate);
  }

  return pressed;
}

bool dialog_button(const Look *k, civue_id id, const char *label,
                   bool primary) {
  bool pressed = false;

  div(key(id), flex, items_center, justify_center, flex_1, h(10), px(4),
      rounded(lg), style(.bg = primary ? k->brand : k->surface_high),
      hover(bg(primary ? k->brand_hover : k->hover)),
      text_color(primary ? CIVUE_WHITE : k->strong), cursor_pointer) {
    pressed = button().activated;
    text(label, font_weight(medium));
  }

  return pressed;
}

void window_handle(civue_id id) {
  civue_ui *ui = civue_ui_current();
  // a region counts as pressed wherever the pointer is over it, so the one
  // under the pointer is asked too: a button of the node keeps its press.
  bool topmost = ui->hovered == id;
  civue_response handle = civue_interact(ui, id, here(), CIVUE_SENSE_CLICK);

  if (!handle.pressed || !topmost) {
    return;
  }

  if (handle.double_clicked) {
    ui->toggle_maximize = true;
  }
  else {
    ui->drag_window = true;
  }
}
