#include "widgets.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <civue/dsl.h>

#include "components.h"
#include "discord/emoji.h"
#include "discord/cdn.h"
#include "explore.h"

typedef struct {
  App *app;
  const char *const *types;
  uint32_t n;
  bool multiple;
  FilesPicked done;
  uint64_t arg;
} PickJob;

static void pick_now(void *ctx) {
  PickJob *job = ctx;
  civue_strs files = civue_file_panel_open(job->types, job->n, job->multiple);

  job->done(job->app, files, job->arg);

  for (uint32_t i = 0; i < files.len; i++) {
    free(files.v[i]);
  }

  free(files.v);
  free(job);
  civue_window_wake();
}

void pick_files_later(App *app, const char *const *types, uint32_t n,
                      bool multiple, FilesPicked done, uint64_t arg) {
  PickJob *job = malloc(sizeof *job);

  *job = (PickJob){app, types, n, multiple, done, arg};
  platform_post(pick_now, job);
}

typedef struct {
  char *name;
  FileSaved done;
  void *ctx;
} SaveJob;

static void save_now(void *ctx) {
  SaveJob *job = ctx;

  job->done(civue_file_panel_save(job->name), job->ctx);
  free(job->name);
  free(job);
  civue_window_wake();
}

void save_file_later(const char *name, FileSaved done, void *ctx) {
  SaveJob *job = malloc(sizeof *job);

  *job = (SaveJob){strdup(name), done, ctx};
  platform_post(save_now, job);
}

void glyph(civue_icon icon, float size, civue_color color) {
  civue_node_icon(civue_ui_current(), icon,
                  &CIVUE_NODE_STYLE(.w = civue_px(size), .h = civue_px(size),
                                    .shrink = 0, .has_shrink = true,
                                    .text_color = color,
                                    .has_text_color = true));
}

bool image_fill_at(App *app, const char *url, float radius, float side) {
  civue_ui *ui = civue_ui_current();
  civue_rect r = here();

  if (side <= 0 || civue_rect_w(r) <= 0 || civue_rect_h(r) <= 0) {
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

bool image_fill(App *app, const char *url, float radius) {
  civue_rect r = here();

  return image_fill_at(app, url, radius,
                       (float)fmax(civue_rect_w(r), civue_rect_h(r)));
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

bool emoji_image(App *app, Snowflake emoji_id, bool animated, civue_rect r) {
  civue_ui *ui = civue_ui_current();
  uint32_t pixels =
    (uint32_t)(fmax(civue_rect_w(r), civue_rect_h(r)) * ui->scale);
  char url[CDN_URL_MAX];
  civue_image_info image;

  if (pixels == 0) {
    return false;
  }

  // one url for both, so the still of an emoji that moves is its first
  // frame without another fetch. its frames are asked at the size the cdn
  // serves: civue keeps only the first of the ones it would have to shrink
  // under 64 pixels.
  if (animated) {
    cdn_emoji_animated(url, emoji_id, pixels);
    image = ui->window_focused
              ? civue_images_get_animated(app->images, ui, url,
                                          cdn_size(pixels))
              : civue_images_get(app->images, ui, url, pixels);
  }
  else {
    cdn_emoji(url, emoji_id, pixels);
    image = civue_images_get(app->images, ui, url, pixels);
  }

  if (image.state != CIVUE_IMAGE_READY || !image.width || !image.height) {
    return false;
  }

  // whole in its square, as the client's object-fit: contain.
  double w = civue_rect_w(r), h = civue_rect_h(r);
  double shape = (double)image.width / (double)image.height;
  double iw = shape >= 1 ? w : h * shape, ih = shape >= 1 ? w / shape : h;
  civue_rect at = xywh(r.x0 + (w - iw) / 2, r.y0 + (h - ih) / 2, iw, ih);

  civue_image(ui, image.texture, at, (float[4]){0, 0, 1, 1}, 1, 0, false);
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

static const Member *member_of(App *app, const User *user,
                               Snowflake guild_id) {
  const Guild *guild =
    guild_id ? store_guild(app->session->store, guild_id) : nullptr;

  return guild ? store_member(guild, user->id) : nullptr;
}

void avatar_url(App *app, const User *user, Snowflake guild_id,
                uint32_t pixels, char *out) {
  const Member *member = user ? member_of(app, user, guild_id) : nullptr;

  out[0] = 0;

  if (member && member->avatar) {
    cdn_member_avatar(out, guild_id, user->id, member->avatar, pixels);
  }
  else if (user) {
    cdn_avatar(out, user->id, user->avatar, user->discriminator, pixels);
  }
}

void avatar(App *app, const User *user, Snowflake guild_id, float size,
            bool status, civue_color ring) {
  char url[CDN_URL_MAX] = "", frame[CDN_URL_MAX] = "";

  avatar_url(app, user, guild_id, (uint32_t)(size * 2), url);

  if (user) {
    const Member *member = member_of(app, user, guild_id);
    const char *worn =
      member && member->decoration ? member->decoration : user->decoration;

    // discord draws it a fifth wider than the avatar it frames.
    cdn_avatar_decoration(frame, worn, (uint32_t)(size * 1.2f * 2));
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

void guild_tag_pill(App *app, const char *tag, const char *badge) {
  const Look *k = &app->look;

  div(flex, items_center, gap(1), flex_shrink_0, h(px(18)), px(1), rounded(sm),
      bg(k->hover)) {
    if (badge && *badge) {
      div(size(px(14)), flex_shrink_0) {
        image_fill(app, badge, 0);
      }
    }

    text(tag, text_size(px(12)), line_height(px(16)), font_weight(semibold),
         text_color(k->text), whitespace_nowrap);
  }
}

void tag_chip(App *app, const User *u) {
  char url[CDN_URL_MAX];

  if (!u->tag || !*u->tag) {
    return;
  }

  cdn_tag_badge(url, u->tag_guild_id, u->tag_badge, 32);
  guild_tag_pill(app, u->tag, url);
}

void mention_dot(const Look *k, civue_color under) {
  civue_rect r = here();

  fill(xywh(r.x1 - 5, r.y0 - 3, 10, 10), under, 5);
  fill(xywh(r.x1 - 3, r.y0 - 1, 6, 6), k->danger, 3);
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
         tabular_nums, text_color(k->on_danger));
  }
}

civue_rect overlay_room(void) {
  civue_ui *ui = civue_ui_current();
  civue_rect s = ui->screen;

  ui->overlay_outside = true;
  return (civue_rect){s.x0 + 8, s.y0 + 8, s.x1 - 8, s.y1 - 8};
}

// the triangle is laid as thin strips, civue draws no triangle.
void tooltip_arrow(civue_rect box, civue_rect anchor, civue_tooltip_side side,
                   civue_color color) {
  double mid_x = (anchor.x0 + anchor.x1) / 2;
  double mid_y = (anchor.y0 + anchor.y1) / 2;
  double x = fmin(fmax(mid_x, box.x0 + 10), box.x1 - 10);
  double y = fmin(fmax(mid_y, box.y0 + 8), box.y1 - 8);

  for (int i = 0; i < 10; i++) {
    double half = 5 - i * .5, step = i * .5;

    if (side == CIVUE_TOOLTIP_ABOVE) {
      fill(xywh(x - half, box.y1 + step, half * 2, .5), color, 0);
    }
    else if (side == CIVUE_TOOLTIP_BELOW) {
      fill(xywh(x - half, box.y0 - step - .5, half * 2, .5), color, 0);
    }
    else {
      fill(xywh(box.x0 - step - .5, y - half, .5, half * 2), color, 0);
    }
  }
}

void hint(App *app, civue_id id, const civue_response *resp, const char *label,
          civue_tooltip_side side) {
  if (!tooltip(id, resp)) {
    return;
  }

  const Look *k = &app->look;
  civue_id tip = id_in(id, "tip");
  civue_tooltip_place at = tooltip_at(resp->rect, last_size(tip), side, 8);
  civue_rect box = at.rect;

  // shadcn's: the text's color under the page's, 12 point, 12 by 6 of
  // padding, and an arrow to what it names.
  overlay() {
    div(key(tip), left(px(at.rect.x0)), top(px(at.rect.y0)), px(3),
        py(px(6)), rounded(lg), bg(k->tooltip), whitespace_nowrap,
        text_size(px(12)), line_height(px(16)), text_color(k->on_tooltip)) {
      box = here();
      text(label);
    }

    tooltip_arrow(box, resp->rect, at.side, k->tooltip);
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
    .bg = k->field,
    .border = k->border_strong,
    .border_width = 1,
    .radius = 8,
    .text = k->text,
  };

  civue_field_box editing = idle;
  editing.border = k->ring;

  return (civue_field_look){
    .pad = 12,
    .editing = editing,
    .idle = idle,
    .hovered = idle,
    .placeholder = k->muted,
    .selection = k->selection,
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
      return CIVUE_ICON(mingcute, music_fill);
    case ACTIVITY_WATCHING:
      return CIVUE_ICON(mingcute, tv_2_fill);
    case ACTIVITY_STREAMING:
      return CIVUE_ICON(mingcute, base_station_fill);
    case ACTIVITY_COMPETING:
      return CIVUE_ICON(mingcute, trophy_fill);
    default:
      return CIVUE_ICON(mingcute, game_2_fill);
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

void header_end(App *app) {
  civue_rect bar = here();

  inbox_button(app);

  float room = (float)(bar.x1 - app->notch.x0);

  if (app->notch.y1 > bar.y0 && room > 0) {
    div(flex_shrink_0, w(px(room))) {}
  }
}

void captcha_start(Captcha *c, const char *sitekey, const char *rqdata) {
  if (c->h) {
    return;
  }

  c->h = hcaptcha_new(&(hcaptcha_options){
    .sitekey = sitekey,
    .origin = "https://discord.com",
    .rqdata = rqdata,
    .theme = "dark",
  });
  c->was = HCAPTCHA_LOADING;
}

void captcha_drop(Captcha *c) {
  hcaptcha_free(c->h);
  c->h = nullptr;
}

// the page is shown by the status of the last frame: hcaptcha only tells it
// opened once it has.
hcaptcha_state captcha_node(App *app, Captcha *c, civue_id key) {
  civue_ui *ui = civue_ui_current();
  const Look *k = &app->look;
  bool open = c->was == HCAPTCHA_OPEN;
  bool live = open || c->was == HCAPTCHA_LOADING;
  hcaptcha_state state = {};

  if (open) {
    overlay() {
      div(key(id("captcha-backdrop")), left(0), top(0), w(px(ui->size.x)),
          h(px(ui->size.y)), bg(k->scrim)) {
        state = hcaptcha_node(ui, c->h);
      }
    }
  }

  // webkit holds back the timers of a page off the screen, so a loading one
  // sits clear over the spinner.
  div(key(key), w(full), h(px(live ? 48 : 0)), flex, items_center,
      justify_center) {
    if (!open) {
      state = hcaptcha_node(ui, c->h);
    }

    if (live) {
      explore_spinner(k->subtle, 32);
    }
  }

  c->was = state.status;
  return state;
}

bool captcha_unshowable(hcaptcha_state s) {
  return s.status == HCAPTCHA_FAILED && s.error &&
         !strcmp(s.error, "no webview on this platform");
}
