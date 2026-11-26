#include "components.h"

#include <math.h>
#include <stdcountof.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <civue/dsl.h>

#include "discord/cdn.h"
#include "widgets.h"

// the sizes of discord's popout and of its full profile, from its
// stylesheets.
constexpr float CARD_WIDTH = 300;
constexpr float BANNER = 105;
constexpr float AVATAR = 80;
constexpr float AVATAR_LEFT = 16;
constexpr float AVATAR_TOP = 61;
constexpr float CUTOUT = 6;
constexpr float THEME_PADDING = 2;
constexpr float FOOTER = 56;
constexpr float EDGE = 12;
constexpr float SLIDE = 10;
constexpr float ROLES_WIDTH = 268;

constexpr float MODAL_WIDTH = 962;
constexpr float MODAL_HEIGHT = 800;
constexpr float MODAL_MIN_HEIGHT = 550;
constexpr float MODAL_BANNER = 140;
constexpr float MODAL_AVATAR = 120;
constexpr float MODAL_CUTOUT = 8;

// discord's default color of a role without one.
constexpr uint32_t ROLE_DEFAULT = 0x99aab5;

// the colors a profile is drawn in, its theme's or the app's.
typedef struct {
  bool themed, light;
  // the theme's two colors, then the same under the overlay that keeps
  // text readable.
  civue_color primary, secondary, start, end;
  civue_color text, strong, subtle, faint;
  // what sits on the card: a chip, a field, and their edges.
  civue_color chip, field, border, hover;
} Paint;

/* ========================================================================== */
/* opening.                                                                   */
/* ========================================================================== */

void profile_open(App *app, Snowflake user_id, Snowflake guild_id,
                  civue_rect anchor, ProfileSide side) {
  bool same = app->popover == POPOVER_PROFILE &&
              app->profile_user == user_id &&
              fabs(app->profile_anchor.x0 - anchor.x0) < 1 &&
              fabs(app->profile_anchor.y0 - anchor.y0) < 1;

  if (same) {
    app->popover = POPOVER_NONE;
    return;
  }

  app->popover = POPOVER_PROFILE;
  app->profile_user = user_id;
  app->profile_guild = guild_id;
  app->profile_anchor = anchor;
  app->profile_side = side;
  app->profile_menu = false;
  app->profile_roles = false;
  app->profile_opens++;

  free(app->profile_message);
  app->profile_message = nullptr;
}

static bool trigger(App *app, Snowflake user_id, Snowflake guild_id,
                    ProfileSide side, bool underline) {
  if (user_id == 0) {
    return false;
  }

  civue_ui *ui = civue_ui_current();
  civue_button_state state = button();
  civue_rect r = here();

  // a name is underlined while it may be clicked, as discord's are.
  if (underline && state.response.hovered) {
    civue_text_style st = civue_node_text_style(ui);

    civue_hline(ui, r.x0, r.x1, (r.y0 + r.y1) / 2 + st.size * .5,
                civue_node_color(ui));
  }

  if (state.activated) {
    profile_open(app, user_id, guild_id, r, side);
  }

  return state.activated;
}

bool profile_trigger(App *app, Snowflake user_id, Snowflake guild_id,
                     ProfileSide side) {
  return trigger(app, user_id, guild_id, side, false);
}

bool profile_name_trigger(App *app, Snowflake user_id, Snowflake guild_id) {
  return trigger(app, user_id, guild_id, PROFILE_RIGHT, true);
}

static void open_modal(App *app, Snowflake user_id, Snowflake guild_id,
                       ProfileTab tab) {
  app->popover = POPOVER_NONE;
  app->modal_user = user_id;
  app->modal_guild = guild_id;
  app->modal_tab = tab;
  app->profile_opens++;

  free(app->profile_note);
  app->profile_note = nullptr;
  app->note_user = 0;
}

/* ========================================================================== */
/* colors.                                                                    */
/* ========================================================================== */

static civue_color mix(civue_color a, civue_color b, float t) {
  return (civue_color){a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t,
                       a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
}

// the theme counts for nitro's highest tier only, the member's theme in a
// guild before the user's.
static Paint paint_of(App *app, const Profile *p) {
  const Look *k = &app->look;
  Paint c = {
    .text = k->text,
    .strong = k->strong,
    .subtle = k->subtle,
    .faint = k->faint,
    .chip = k->hover,
    .field = k->input,
    .border = k->border,
    .hover = k->hover,
  };
  const uint32_t *theme = p->has_guild_theme ? p->guild_theme
                          : p->has_theme     ? p->theme
                                             : nullptr;

  if (theme == nullptr || p->premium_type != 2) {
    return c;
  }

  civue_color primary = rgb(theme[0]);
  float luma = (.299f * primary.r + .587f * primary.g + .114f * primary.b);

  // discord picks the dark look over a dark primary color.
  c.themed = true;
  c.light = 1 - luma <= .5f;
  c.primary = primary;
  c.secondary = rgb(theme[1]);

  civue_color over = c.light ? CIVUE_WHITE : CIVUE_BLACK;
  civue_color ink = c.light ? CIVUE_BLACK : CIVUE_WHITE;

  c.start = mix(c.primary, over, .6f);
  c.end = mix(c.secondary, over, .6f);
  c.text = civue_alpha(ink, .96f);
  c.strong = civue_alpha(ink, .96f);
  c.subtle = civue_alpha(ink, .88f);
  c.faint = civue_alpha(ink, .76f);
  c.chip = c.light ? rgba(0xffffff3d) : rgba(0x97979f29);
  c.field = c.light ? rgba(0x00000014) : rgba(0xffffff1f);
  c.border = civue_alpha(ink, .08f);
  c.hover = civue_alpha(ink, .08f);

  return c;
}

// `top` down to `from`, `bottom` from `to` on, and the one turning into the
// other between them, inside `r` with its top corners rounded by `radius`
// and its bottom ones by `under`.
static void gradient(civue_rect r, float radius, float under, civue_color top,
                     civue_color bottom, double from, double to) {
  civue_ui *ui = civue_ui_current();
  float radii[4] = {radius, radius, under, under};

  civue_push_round(ui, r, radii);

  CIVUE_CLIPPED(ui, r) {
    civue_fill(ui, r, top, 0);

    if (to > from) {
      civue_fade_out(ui, civue_rect_from(r.x0, from, r.x1, to), bottom);
    }

    civue_fill(ui, civue_rect_from(r.x0, to, r.x1, r.y1), bottom, 0);
  }

  civue_pop_round(ui);
}

/* ========================================================================== */
/* pieces both share.                                                         */
/* ========================================================================== */

static const char *banner_hash(const Profile *p) {
  return p->guild_banner ? p->guild_banner : p->banner;
}

static void banner_url(const Profile *p, uint32_t size, char *url) {
  if (p->guild_banner) {
    cdn_member_banner(url, p->guild_id, p->user_id, p->guild_banner, size);
  }
  else {
    cdn_banner(url, p->user_id, p->banner, size);
  }
}

static void avatar_url(App *app, const User *u, Snowflake guild_id,
                       uint32_t size, char *url) {
  const Guild *guild = store_guild(app->session->store, guild_id);
  const Member *member = guild ? store_member(guild, u->id) : nullptr;

  if (member && member->avatar) {
    cdn_member_avatar(url, guild_id, u->id, member->avatar, size);
  }
  else {
    cdn_avatar(url, u->id, u->avatar, u->discriminator, size);
  }
}

// the banner's color without an image: the theme's or the accent, else the
// color most of the avatar is.
static civue_color banner_ground(App *app, const User *u, const Profile *p,
                                 const Paint *c, Snowflake guild_id) {
  if (c->themed) {
    return c->primary;
  }

  if (p->has_accent) {
    return rgb(p->accent_color);
  }

  if (u->has_accent) {
    return rgb(u->accent_color);
  }

  civue_ui *ui = civue_ui_current();
  char url[CDN_URL_MAX];

  avatar_url(app, u, guild_id, 160, url);

  civue_image_info info = civue_images_get(app->images, ui, url, 160);

  return info.color ? rgba(info.color) : app->look.frame;
}

static void banner(App *app, const User *u, const Profile *p, const Paint *c,
                   Snowflake guild_id, float width, float height,
                   float radius) {
  char url[CDN_URL_MAX] = "";
  civue_color ground = banner_ground(app, u, p, c, guild_id);

  if (banner_hash(p)) {
    banner_url(p, width * 2 > 600 ? 1024 : 600, url);
  }

  div(flex_shrink_0, w(full), h(px(height)), rounded_t(px(radius)),
      overflow_hidden,
      style(.bg = *url ? app->look.frame : ground, .has_bg = true)) {
    if (*url) {
      image_cover(app, url, 0);
    }
  }
}

static UserStatus status_of(App *app, const User *u) {
  const Store *store = app->session->store;

  return u->id == store->self_id ? store->self_status : u->status;
}

// the avatar cut out of what is under it by `cutout` points, its status at
// its corner cut the same way. hovered, it darkens as it may be clicked.
static void cut_avatar(App *app, const User *u, Snowflake guild_id, float size,
                       float cutout, float dot, float dot_offset,
                       civue_color ring, bool lit) {
  const Look *k = &app->look;

  div(relative, size(px(size)), flex_shrink_0) {
    civue_rect r = here();

    fill(civue_rect_inflate(r, cutout, cutout), ring, size / 2 + cutout);
    avatar(app, u, guild_id, size, false, ring);

    if (lit) {
      fill(r, rgba(0x00000066), size / 2);
    }

    civue_rect at = xywh(r.x0 + size - dot - dot_offset,
                         r.y0 + size - dot - dot_offset, dot, dot);

    fill(civue_rect_inflate(at, cutout - 3, cutout - 3), ring, dot);
    status_dot(status_of(app, u), at, k, ring);
  }
}

// what the user wrote as their status, in the bubble discord draws beside
// the avatar.
static bool has_custom_status(const User *u) {
  return (u->status_text && *u->status_text) || u->status_emoji;
}

static void status_bubble(App *app, const User *u, const Paint *c) {
  const Look *k = &app->look;
  civue_color ground = c->themed ? mix(c->start, k->surface_high, .5f)
                                 : k->surface_high;
  civue_color edge = c->themed ? c->border : k->border;
  bool words = u->status_text && *u->status_text;

  div(relative, flex, items_start) {
    // the thought's dots, under the bubble.
    div(absolute, left(px(-3)), top(px(-15)), size(px(10)), rounded(full),
        bg(ground), border(1), border_color(edge)) {}
    div(absolute, left(px(10)), top(px(-8)), size(px(20)), rounded(full),
        bg(ground), border(1), border_color(edge)) {}

    div(relative, flex, items_center, gap(1), min_w(px(42)), max_w(px(181)),
        px(3), py(2), rounded(px(16)), bg(ground), border(1),
        border_color(edge), shadow(sm)) {
      if (u->status_emoji) {
        emoji_node(app, u->status_emoji_id, u->status_emoji, words ? 16 : 32);
      }

      if (words) {
        div(min_w(0), max_h(px(36)), overflow_hidden) {
          text(u->status_text, text_size(px(14)), line_height(px(18)),
               text_color(c->text));
        }
      }
    }
  }
}

// the badges, each with its name when pointed at.
static void badges(App *app, civue_id key, const Profile *p, float side) {
  char url[CDN_URL_MAX];

  for (size_t i = 0; i < p->nbadges; i++) {
    const SessionBadge *badge = &p->badges[i];
    civue_id id = id_at(key, i);

    cdn_badge_icon(url, badge->icon);

    div(key(id), size(px(side)), flex_shrink_0) {
      civue_button_state state = button();

      image_fill(app, url, 0);

      if (badge->description) {
        hint(app, id, &state.response, badge->description,
             CIVUE_TOOLTIP_ABOVE);
      }
    }
  }
}

static const char *pronouns_of(const Profile *p, const User *u) {
  const char *s = p->guild_pronouns && *p->guild_pronouns ? p->guild_pronouns
                  : p->pronouns && *p->pronouns           ? p->pronouns
                                                          : u->pronouns;

  return s && *s ? s : nullptr;
}

static const char *bio_of(const Profile *p, const User *u) {
  const char *s = p->guild_bio && *p->guild_bio ? p->guild_bio
                  : p->bio && *p->bio           ? p->bio
                                                : u->bio;

  return s && *s ? s : nullptr;
}

// the display name, then the username, the pronouns, the guild's tag and
// the badges on a line under it. a click on the names calls `pressed`.
static bool names(App *app, civue_id key, const User *u, const Profile *p,
                  const Paint *c, Snowflake guild_id, bool large,
                  bool clickable) {
  Store *store = app->session->store;
  const char *pronouns = pronouns_of(p, u);
  bool pressed = false;

  div(key(key), flex, flex_col, gap(px(large ? 4 : 2)), min_w(0),
      style(.has_cursor = clickable, .cursor = CIVUE_CURSOR_POINTER)) {
    if (clickable) {
      pressed = button().activated;
    }

    text(store_display_name(store, guild_id, u->id),
         text_size(px(large ? 24 : 20)), line_height(px(large ? 30 : 24)),
         font_weight(semibold), text_color(c->strong));

    div(flex, flex_wrap, items_center, gap_x(px(pronouns || u->bot ? 6 : 9.33f)),
        gap_y(px(2)), min_w(0), text_size(px(14)), line_height(px(18)),
        text_color(c->strong)) {
      text(u->username ? u->username : "", min_w(0), truncate);

      if (u->bot) {
        div(flex_shrink_0, px(px(5)), rounded(sm), bg(app->look.primary)) {
          text("APP", text_size(px(12)), line_height(px(16)),
               font_weight(semibold), text_color(app->look.on_primary));
        }
      }

      if (pronouns) {
        div(size(px(4)), mx(px(1)), rounded(full), bg(c->faint)) {}
        text(pronouns, min_w(0), truncate);
      }

      tag_chip(app, u);

      if (!large && p->nbadges) {
        div(flex, items_center, flex_wrap, gap(px(2))) {
          badges(app, id_in(key, "badges"), p, 20);
        }
      }
    }

    if (large && p->nbadges) {
      div(flex, items_center, gap(px(2)), mt(px(4))) {
        badges(app, id_in(key, "badges"), p, 22);
      }
    }
  }

  return pressed;
}

// a section of the full profile: its title over what it holds.
static void heading(const Paint *c, const char *title) {
  text(title, text_size(px(12)), line_height(px(16)), font_weight(medium),
       text_color(c->subtle));
}

static const char *const ACTIVITY_VERBS[] = {
  [ACTIVITY_PLAYING] = "Playing",
  [ACTIVITY_STREAMING] = "Streaming",
  [ACTIVITY_LISTENING] = "Listening to",
  [ACTIVITY_WATCHING] = "Watching",
  [ACTIVITY_COMPETING] = "Competing in",
};

static void activity_card(App *app, civue_id key, const Activity *a,
                          const Paint *c) {
  char url[CDN_URL_MAX], badge[CDN_URL_MAX];
  const char *verb =
    a->type < countof(ACTIVITY_VERBS) ? ACTIVITY_VERBS[a->type] : nullptr;

  cdn_application_icon(
    badge, a->application_id,
    session_application_icon(app->session, a->application_id), 96);
  cdn_activity_image(url, a->application_id, a->large_image, 96);

  if (!*url) {
    snprintf(url, sizeof url, "%s", badge);
  }

  div(key(key), flex, flex_col, gap(2), p(3), rounded(lg), bg(c->chip)) {
    text(verb ? verb : "Activity", text_size(px(12)), line_height(px(16)),
         font_weight(semibold), text_color(c->subtle));

    div(flex, items_center, gap(3), min_w(0)) {
      div(flex, items_center, justify_center, size(px(48)), flex_shrink_0,
          rounded(lg), overflow_hidden, bg(c->hover)) {
        if (*url) {
          image_cover(app, url, 8);
        }
        else {
          glyph(activity_icon(a->type), 24, c->subtle);
        }
      }

      div(flex, flex_col, flex_1, min_w(0)) {
        text(a->name ? a->name : "", truncate, text_size(px(14)),
             line_height(px(18)), font_weight(semibold),
             text_color(c->strong));

        if (a->details) {
          text(a->details, truncate, text_size(px(12)), line_height(px(16)),
               text_color(c->text));
        }

        if (a->state) {
          text(a->state, truncate, text_size(px(12)), line_height(px(16)),
               text_color(c->subtle));
        }
      }
    }
  }
}

static const Activity *first_activity(const User *u) {
  for (size_t i = 0; i < u->nactivities; i++) {
    if (u->activities[i].type != ACTIVITY_CUSTOM) {
      return &u->activities[i];
    }
  }

  return nullptr;
}

// the member's roles, highest first. `out` holds as many as the guild has.
static size_t roles_of(const Guild *guild, const Member *m,
                       const Role **out) {
  size_t n = 0;

  for (size_t i = 0; i < guild->nroles; i++) {
    const Role *r = guild->roles[i];

    for (size_t j = 0; j < m->nroles; j++) {
      if (m->roles[j] == r->id && r->id != guild->id) {
        out[n++] = r;
        break;
      }
    }
  }

  return n;
}

static float role_width(const Role *r) {
  civue_text_style st = civue_style(12, 400, 16, 0);
  float w = civue_layout(civue_ui_current(), r->name ? r->name : "", st, 0)
              ->width;

  return 2 + 20 + 4 + w + 8;
}

static void role_pill(const Role *r, const Paint *c) {
  civue_color dot = rgb(r->color ? r->color : ROLE_DEFAULT);

  div(flex, items_center, gap(1), min_h(px(24)), pl(px(2)), pr(px(8)),
      rounded(lg), border(1), border_color(c->border)) {
    div(flex, items_center, justify_center, size(px(20)), flex_shrink_0) {
      div(size(px(12)), rounded(full), bg(dot)) {}
    }

    text(r->name ? r->name : "", min_w(0), truncate, text_size(px(12)),
         line_height(px(16)), text_color(c->text));
  }
}

// how many of the roles fit in two rows of `width`, with room left for
// the pill that shows the rest.
static size_t roles_fitting(const Role **roles, size_t count, float width) {
  float x = 0, more = 40;
  int row = 0;

  for (size_t i = 0; i < count; i++) {
    float w = role_width(roles[i]);
    float room = row == 1 && i + 1 < count ? width - more : width;

    if (x > 0 && x + w > room) {
      if (++row == 2) {
        return i;
      }

      x = 0;
    }

    x += w + 4;
  }

  return count;
}

/* ========================================================================== */
/* the card.                                                                  */
/* ========================================================================== */

// a round button over the banner.
static bool banner_button(App *app, civue_id id, civue_icon icon,
                          const char *label) {
  bool pressed = false;

  div(key(id), flex, items_center, justify_center, size(px(32)), rounded(full),
      bg(rgba(0x0000008f)), border(1), border_color(rgba(0xffffff14)),
      hover(bg(rgba(0x000000b3))), cursor_pointer) {
    civue_button_state state = button();

    pressed = state.activated;
    glyph(icon, 16, CIVUE_WHITE);
    hint(app, id, &state.response, label, CIVUE_TOOLTIP_ABOVE);
  }

  return pressed;
}

static void relationship_button(App *app, const User *u) {
  Session *s = app->session;
  const Relationship *r = store_relationship(s->store, u->id);
  RelationshipType type = r ? r->type : RELATIONSHIP_NONE;
  civue_id id = id("profile-relationship");

  if (u->bot || u->id == s->store->self_id || type == RELATIONSHIP_BLOCKED) {
    return;
  }

  if (type == RELATIONSHIP_FRIEND) {
    banner_button(app, id, CIVUE_ICON(mingcute, user_follow_fill), "Friend");
  }
  else if (type == RELATIONSHIP_INCOMING) {
    banner_button(app, id, CIVUE_ICON(mingcute, user_search_fill),
                  "Incoming Friend Request");
  }
  else if (type == RELATIONSHIP_OUTGOING) {
    banner_button(app, id, CIVUE_ICON(mingcute, user_search_fill),
                  "Outgoing Friend Request");
  }
  else if (banner_button(app, id, CIVUE_ICON(mingcute, user_add_fill),
                         "Add Friend") &&
           u->username) {
    session_add_friend(s, u->username, nullptr);
  }
}

static void copy_text(const char *s) {
  civue_clipboard_set_text(s, strlen(s));
}

// what the card's more button holds, under `anchor`.
static void card_menu(App *app, const User *u, const Profile *p,
                      civue_rect anchor, civue_rect *card) {
  const Look *k = &app->look;
  civue_ui *ui = civue_ui_current();
  char digits[21];
  bool chosen = false;

  overlay() {
    div(key(id("profile-menu")), flex, flex_col, left(px(anchor.x1 - 220)),
        top(px(anchor.y1 + 8)), w(px(220)), p(2), rounded(lg),
        bg(k->popover), border(1), border_color(k->border_strong),
        shadow(lg)) {
      *card = here();

      if (menu_row(app, id("profile-menu-full"), CIVUE_ICON(mingcute, idcard_fill),
                   "View Full Profile", k->text)) {
        open_modal(app, u->id, p->guild_id, PROFILE_TAB_ACTIVITY);
        chosen = true;
      }

      if (u->username && menu_row(app, id("profile-menu-name"),
                                  CIVUE_ICON(mingcute, at_fill),
                                  "Copy Username", k->text)) {
        copy_text(u->username);
        chosen = true;
      }

      if (menu_row(app, id("profile-menu-id"), CIVUE_ICON(mingcute, copy_2_fill),
                   "Copy User ID", k->text)) {
        snowflake_format(u->id, digits);
        copy_text(digits);
        chosen = true;
      }
    }
  }

  if (chosen || civue_key_pressed(ui, CIVUE_KEY_ESCAPE)) {
    app->profile_menu = false;
  }
}

// the mutual friends and servers in a line, each opening its tab of the
// full profile.
static void mutuals_line(App *app, const User *u, const Profile *p,
                         const Paint *c) {
  Store *store = app->session->store;
  size_t friends = p->mutual_friends_count > p->nmutual_friends
                     ? p->mutual_friends_count
                     : p->nmutual_friends;
  size_t guilds = p->nmutual_guilds;
  char label[48];

  if (u->id == store->self_id || (friends == 0 && guilds == 0)) {
    return;
  }

  float size = friends && guilds ? 12 : 14;

  div(flex, items_center, gap(2), min_w(0), text_size(px(size)),
      line_height(px(size + 4)), text_color(c->subtle)) {
    if (friends) {
      div(key(id("profile-mutual-friends")), flex, items_center, gap(1),
          py(1), cursor_pointer, hover(text_color(c->strong))) {
        if (button().activated) {
          open_modal(app, u->id, p->guild_id, PROFILE_TAB_FRIENDS);
        }

        div(flex, items_center) {
          for (size_t i = 0; i < p->nmutual_friends && i < 3; i++) {
            const User *f = store_user(store, p->mutual_friends[i]);

            div(ml(px(i ? -4 : 0)), rounded(full), border(2),
                border_color(c->themed ? c->start : app->look.surface)) {
              avatar(app, f, 0, 16, false, app->look.surface);
            }
          }
        }

        snprintf(label, sizeof label, "%zu Mutual Friend%s", friends,
                 friends == 1 ? "" : "s");
        text(label, whitespace_nowrap);
      }
    }

    if (friends && guilds) {
      div(size(px(4)), rounded(full), bg(c->faint), flex_shrink_0) {}
    }

    if (guilds) {
      div(key(id("profile-mutual-guilds")), flex, items_center, gap(1), py(1),
          cursor_pointer, hover(text_color(c->strong))) {
        if (button().activated) {
          open_modal(app, u->id, p->guild_id, PROFILE_TAB_GUILDS);
        }

        if (!friends) {
          div(flex, items_center) {
            for (size_t i = 0; i < guilds && i < 3; i++) {
              const Guild *g = store_guild(store, p->mutual_guilds[i].id);

              if (g) {
                div(ml(px(i ? -4 : 0)), size(px(16)), rounded(sm),
                    overflow_hidden) {
                  guild_icon(app, g, 16, false);
                }
              }
            }
          }
        }

        snprintf(label, sizeof label, "%zu Mutual Server%s", guilds,
                 guilds == 1 ? "" : "s");
        text(label, whitespace_nowrap);
      }
    }
  }
}

static void card_roles(App *app, const User *u, const Paint *c,
                       Snowflake guild_id) {
  const Guild *guild = store_guild(app->session->store, guild_id);
  const Member *m = guild ? store_member(guild, u->id) : nullptr;

  if (!m || guild->nroles == 0) {
    return;
  }

  const Role **roles = calloc(guild->nroles, sizeof *roles);
  size_t count = roles_of(guild, m, roles);
  size_t shown = app->profile_roles ? count
                                    : roles_fitting(roles, count, ROLES_WIDTH);
  char more[24];

  if (count) {
    div(flex, flex_wrap, gap(1)) {
      for (size_t i = 0; i < shown; i++) {
        role_pill(roles[i], c);
      }

      if (count > shown || app->profile_roles) {
        if (app->profile_roles) {
          snprintf(more, sizeof more, "Collapse");
        }
        else {
          snprintf(more, sizeof more, "+%zu", count - shown);
        }

        div(key(id("profile-roles-more")), flex, items_center, min_h(px(24)),
            px(2), rounded(lg), border(1), border_color(c->border),
            hover(bg(c->hover)), cursor_pointer) {
          civue_button_state state = button();

          if (state.activated) {
            app->profile_roles = !app->profile_roles;
          }

          text(more, text_size(px(12)), line_height(px(16)),
               text_color(c->text));
          hint(app, id("profile-roles-more"), &state.response,
               app->profile_roles ? "Collapse Roles" : "View All Roles",
               CIVUE_TOOLTIP_ABOVE);
        }
      }
    }
  }

  free(roles);
}

static void card_bio(App *app, const User *u, const Profile *p,
                     const Paint *c) {
  const char *bio = bio_of(p, u);
  civue_id key = id_at(id("profile-bio"), u->id);

  if (!bio) {
    return;
  }

  float full = (float)last_size(key).y;
  bool clamped = full > 57.75f;

  div(flex, flex_col, items_start) {
    div(w(full), style(.max_h = civue_px(clamped ? 55 : 1e6f)),
        overflow_hidden) {
      markup(app, key, bio,
             &(MarkupOptions){.guild_id = p->guild_id,
                              .size = 14,
                              .color = c->strong,
                              .hint = CARD_WIDTH - 32});
    }

    if (clamped) {
      div(key(id("profile-full-bio")), mt(2), px(2), h(px(24)), flex,
          items_center, rounded(md), bg(c->chip), hover(bg(c->hover)),
          text_size(px(12)), line_height(px(16)), font_weight(medium),
          text_color(c->strong), cursor_pointer) {
        if (button().activated) {
          open_modal(app, u->id, p->guild_id, PROFILE_TAB_ACTIVITY);
        }

        text("View Full Bio");
      }
    }
  }
}

static bool shares_place(App *app, const User *u, const Profile *p) {
  const Relationship *r = store_relationship(app->session->store, u->id);

  return (r && r->type == RELATIONSHIP_FRIEND) || p->nmutual_guilds > 0 ||
         p->guild_id != 0;
}

static void send_message(App *app, Snowflake user_id) {
  char *message = app->profile_message;

  app->profile_message = nullptr;
  app->popover = POPOVER_NONE;
  app_select_channel(app, 0);
  session_message_user(app->session, user_id, message);
  free(message);
}

static civue_field_look message_look(const App *app, const Paint *c) {
  civue_field_look look = field_look(&app->look);

  look.idle.bg = look.hovered.bg = look.editing.bg = c->field;
  look.idle.border = look.hovered.border = c->border;
  look.idle.text = look.hovered.text = look.editing.text = c->text;
  look.placeholder = c->faint;

  return look;
}

static void card_footer(App *app, const User *u, const Profile *p,
                        const Paint *c) {
  Store *store = app->session->store;
  char placeholder[160];

  if (u->id == store->self_id || u->bot || !shares_place(app, u, p)) {
    return;
  }

  snprintf(placeholder, sizeof placeholder, "Message @%s",
           store_display_name(store, p->guild_id, u->id));

  div(flex_shrink_0, h(px(44)), mx(4), mb(3), text_size(px(14)),
      line_height(px(18))) {
    civue_field_response field =
      text_field(&app->profile_message, placeholder, message_look(app, c));

    if (field.submitted && app->profile_message && *app->profile_message) {
      send_message(app, u->id);
    }
  }
}

static void card_body(App *app, const User *u, const Profile *p,
                      const Paint *c) {
  const Relationship *r = store_relationship(app->session->store, u->id);
  const Activity *a = first_activity(u);

  div(key(id("profile-body")), flex, flex_col, gap(3), flex_shrink, min_h(0),
      overflow_y_scroll, scrollbars(&app->thin_bars), pt(1), px(4), pb(2)) {
    if (p->failed) {
      div(flex, items_center, gap(2), text_size(px(12)), line_height(px(16)),
          text_color(app->look.critical_text)) {
        icon(mingcute, alert_fill, size(px(14)));
        text("Unable to load parts of profile");
      }
    }

    if (names(app, id("profile-names"), u, p, c, p->guild_id, false, true)) {
      open_modal(app, u->id, p->guild_id, PROFILE_TAB_ACTIVITY);
    }

    if (r && (r->type == RELATIONSHIP_BLOCKED || r->ignored)) {
      text(r->type == RELATIONSHIP_BLOCKED ? "You blocked them"
                                           : "You ignored them",
           text_size(px(14)), line_height(px(18)), text_color(c->subtle));
    }

    mutuals_line(app, u, p, c);
    card_bio(app, u, p, c);

    if (a) {
      activity_card(app, id_at(id("profile-activity"), u->id), a, c);
    }

    card_roles(app, u, c, p->guild_id);
  }
}

// the card's place by its anchor: beside it on its side, or on the other
// one when that hides less of it, pushed back inside the window.
static civue_rect card_place(App *app, civue_vec2 size, bool *flipped) {
  civue_ui *ui = civue_ui_current();
  civue_rect a = app->profile_anchor;
  bool left = app->profile_side == PROFILE_LEFT;
  float gap = left ? 16 : 8;
  float right_x = (float)a.x1 + gap, left_x = (float)a.x0 - gap - size.x;
  float over_right = fmaxf(0, right_x + size.x - (ui->size.x - EDGE));
  float over_left = fmaxf(0, EDGE - left_x);
  float x = left ? left_x : right_x;

  *flipped = left ? over_left > over_right : over_right > over_left;

  if (*flipped) {
    x = left ? right_x : left_x;
  }

  x = fminf(fmaxf(x, EDGE), ui->size.x - EDGE - size.x);

  float y = fminf((float)a.y0, ui->size.y - EDGE - size.y);

  y = fmaxf(y, EDGE);

  return xywh(x, y, size.x, size.y);
}

void profile_popout(App *app) {
  if (app->popover != POPOVER_PROFILE) {
    app->profile_menu = false;
    return;
  }

  civue_ui *ui = civue_ui_current();
  Session *s = app->session;
  const User *u = store_user(s->store, app->profile_user);

  if (u == nullptr) {
    app->popover = POPOVER_NONE;
    return;
  }

  const Look *k = &app->look;
  const Profile *p =
    session_profile(s, app->profile_user, app->profile_guild, false);
  Paint c = paint_of(app, p);
  float pad = c.themed ? THEME_PADDING : 0;
  civue_id id = id("profile-card");
  civue_vec2 size = civue_node_last_size(ui, id);
  bool flipped = false;

  if (size.x == 0) {
    size = civue_v2(CARD_WIDTH + pad * 2, 360);
  }

  civue_rect at = card_place(app, size, &flipped);
  bool right = (app->profile_side == PROFILE_RIGHT) != flipped;
  float slide = civue_animate_from(ui, id_at(id("profile-slide"),
                                             app->profile_opens),
                                   right ? SLIDE : -SLIDE, 0, .2f);
  civue_rect card = {}, menu = {}, more = {};
  bool open_menu = false, full = false;

  overlay() {
    div(key(id), flex, flex_col, left(px(at.x0 + slide)), top(px(at.y0)),
        w(px(CARD_WIDTH + pad * 2)), max_h(px(ui->size.y - 20)), p(px(pad)),
        rounded(lg), shadow(lg), text_color(c.text), text_size(px(14)),
        line_height(px(18)),
        style(.bg = c.themed ? CIVUE_CLEAR : k->surface,
              .has_bg = !c.themed)) {
      card = here();
      // a press on the card is not one on what is behind it.
      button();

      if (c.themed) {
        gradient(card, 8, 8, c.primary, c.secondary, card.y0 + BANNER + pad,
                 card.y1 - FOOTER);
      }

      div(relative, flex, flex_col, flex_shrink, min_h(0),
          rounded(px(8 - pad)), overflow_hidden) {
        civue_rect inner = here();

        if (c.themed) {
          gradient(inner, 8 - pad, 8 - pad, c.start, c.end, inner.y0 + BANNER,
                   inner.y1);
        }

        civue_color ring = c.themed ? c.start : k->surface;

        div(relative, flex_shrink_0, h(px(BANNER + 35 - (c.themed ? 2 : 0)))) {
          banner(app, u, p, &c, app->profile_guild, CARD_WIDTH, BANNER,
                 8 - pad);

          div(absolute, top(px(8)), right(px(12)), flex, gap(2)) {
            relationship_button(app, u);
            open_menu = banner_button(app, id("profile-more"),
                                      CIVUE_ICON(mingcute, more_1_fill), "More");
            more = here();
          }

          div(key(id("profile-avatar")), absolute,
              left(px(AVATAR_LEFT - pad)), top(px(AVATAR_TOP - pad)),
              cursor_pointer) {
            civue_button_state state = button();

            full = state.activated;
            cut_avatar(app, u, app->profile_guild, AVATAR, CUTOUT, 16, 4,
                       ring, state.response.hovered);
            hint(app, id("profile-avatar"), &state.response,
                 "View Full Profile", CIVUE_TOOLTIP_ABOVE);
          }

          if (has_custom_status(u)) {
            div(absolute, left(px(105 - pad)), top(px(BANNER - 7 - pad)),
                right(px(12))) {
              status_bubble(app, u, &c);
            }
          }
        }

        card_body(app, u, p, &c);
        card_footer(app, u, p, &c);
      }

      civue_stroke(ui, civue_rect_inflate(card, 1, 1), app->look.border, 1, 9);
    }
  }

  if (open_menu) {
    app->profile_menu = !app->profile_menu;
  }

  if (app->profile_menu && app->popover == POPOVER_PROFILE) {
    card_menu(app, u, p, more, &menu);
  }

  if (full) {
    open_modal(app, u->id, app->profile_guild, PROFILE_TAB_ACTIVITY);
    return;
  }

  civue_vec2 mouse = ui->input.mouse;
  bool outside = ui->input.pressed[0] && ui->input.has_mouse &&
                 !civue_rect_contains(card, mouse) &&
                 !civue_rect_contains(app->profile_anchor, mouse) &&
                 !(app->profile_menu && civue_rect_contains(menu, mouse));

  if (outside) {
    app->popover = POPOVER_NONE;
  }
  else if (civue_key_pressed(ui, CIVUE_KEY_ESCAPE)) {
    if (app->profile_menu) {
      app->profile_menu = false;
    }
    else {
      app->popover = POPOVER_NONE;
    }
  }
}

/* ========================================================================== */
/* the full profile.                                                          */
/* ========================================================================== */

static void modal_buttons(App *app, const User *u, const Profile *p,
                          const Paint *c) {
  const Look *k = &app->look;
  Session *s = app->session;
  const Relationship *r = store_relationship(s->store, u->id);
  RelationshipType type = r ? r->type : RELATIONSHIP_NONE;
  bool self = u->id == s->store->self_id;
  bool friends = type == RELATIONSHIP_FRIEND || type == RELATIONSHIP_OUTGOING;
  bool message = false, add = false;
  // a themed profile's button is its own color darkened, under white.
  civue_color primary = c->themed ? mix(c->primary, CIVUE_BLACK, .35f)
                                  : k->primary;
  civue_color lit =
    c->themed ? mix(primary, CIVUE_BLACK, .1f) : k->primary_hover;
  civue_color ink = c->themed ? CIVUE_WHITE : k->on_primary;

  div(flex, items_center, gap(2), flex_wrap) {
    if (!self && !friends && !u->bot && type != RELATIONSHIP_BLOCKED) {
      div(key(id("modal-add")), flex, items_center, gap(2), h(8), px(3),
          rounded(lg), bg(primary), hover(bg(lit)), text_color(ink),
          font_weight(medium), cursor_pointer) {
        add = button().activated;
        icon(mingcute, user_add_fill, size(px(16)));
        text(type == RELATIONSHIP_INCOMING ? "Accept" : "Add Friend");
      }

      div(key(id("modal-message")), flex, items_center, justify_center,
          size(8), rounded(lg), bg(c->chip), hover(bg(c->hover)),
          text_color(c->strong), cursor_pointer) {
        civue_button_state state = button();

        message = state.activated;
        icon(mingcute, chat_3_fill, size(px(16)));
        hint(app, id("modal-message"), &state.response, "Message",
             CIVUE_TOOLTIP_ABOVE);
      }
    }
    else {
      div(key(id("modal-message")), flex, items_center, gap(2), h(8), px(3),
          rounded(lg), font_weight(medium),
          style(.bg = self ? civue_alpha(primary, .5f) : primary,
                .has_bg = true,
                .text_color = civue_alpha(ink, self ? .5f : 1),
                .has_text_color = true, .has_cursor = !self,
                .cursor = CIVUE_CURSOR_POINTER),
          hover(bg(self ? civue_alpha(primary, .5f) : lit))) {
        message = button().activated && !self;
        icon(mingcute, chat_3_fill, size(px(16)));
        text("Message");
      }

      if (type == RELATIONSHIP_FRIEND) {
        div(key(id("modal-friend")), flex, items_center, justify_center,
            size(8), rounded(lg), bg(c->chip), text_color(c->strong)) {
          civue_button_state state = button();

          icon(mingcute, user_follow_fill, size(px(16)));
          hint(app, id("modal-friend"), &state.response, "Friend",
               CIVUE_TOOLTIP_ABOVE);
        }
      }
    }
  }

  if (add && u->username) {
    session_add_friend(s, u->username, nullptr);
  }

  if (message) {
    app->modal_user = 0;
    app_select_channel(app, 0);
    session_open_dm(s, u->id);
  }

  (void)p;
}

static void member_since(App *app, const User *u, const Profile *p,
                         const Paint *c) {
  const Guild *guild = store_guild(app->session->store, p->guild_id);
  const Member *m = guild ? store_member(guild, u->id) : nullptr;
  char day[48];

  div(flex, flex_col, gap(2)) {
    heading(c, "Member Since");

    div(flex, items_center, gap(2), text_size(px(14)), line_height(px(18)),
        text_color(c->text)) {
      div(key(id("modal-since-discord")), flex, items_center) {
        civue_button_state state = button();

        icon(simple_icons, discord, size(px(16)), text_color(c->subtle));
        hint(app, id("modal-since-discord"), &state.response, "Discord",
             CIVUE_TOOLTIP_ABOVE);
      }

      format_short_day(snowflake_time_ms(u->id), day, sizeof day);
      text(day);

      if (m && m->joined_at) {
        div(size(px(4)), rounded(full), bg(c->faint)) {}

        div(key(id("modal-since-guild")), size(px(16)), rounded(sm),
            overflow_hidden) {
          civue_button_state state = button();

          guild_icon(app, guild, 16, false);
          hint(app, id("modal-since-guild"), &state.response,
               guild->name ? guild->name : "", CIVUE_TOOLTIP_ABOVE);
        }

        format_short_day(m->joined_at, day, sizeof day);
        text(day);
      }
    }
  }
}

// the icon of an account elsewhere by its type, a globe for what has none.
static civue_icon connection_icon(const char *type) {
  static const struct {
    const char *type;
    civue_icon icon;
  } icons[] = {
    {"github", CIVUE_ICON(simple_icons, github)},
    {"twitch", CIVUE_ICON(simple_icons, twitch)},
    {"youtube", CIVUE_ICON(simple_icons, youtube)},
    {"spotify", CIVUE_ICON(simple_icons, spotify)},
    {"steam", CIVUE_ICON(simple_icons, steam)},
    {"reddit", CIVUE_ICON(simple_icons, reddit)},
    {"twitter", CIVUE_ICON(simple_icons, x)},
    {"xbox", CIVUE_ICON(simple_icons, xbox)},
    {"playstation", CIVUE_ICON(simple_icons, playstation)},
    {"battlenet", CIVUE_ICON(simple_icons, battledotnet)},
    {"epicgames", CIVUE_ICON(simple_icons, epicgames)},
    {"tiktok", CIVUE_ICON(simple_icons, tiktok)},
    {"instagram", CIVUE_ICON(simple_icons, instagram)},
    {"facebook", CIVUE_ICON(simple_icons, facebook)},
    {"paypal", CIVUE_ICON(simple_icons, paypal)},
    {"ebay", CIVUE_ICON(simple_icons, ebay)},
    {"bluesky", CIVUE_ICON(simple_icons, bluesky)},
    {"crunchyroll", CIVUE_ICON(simple_icons, crunchyroll)},
    {"roblox", CIVUE_ICON(simple_icons, roblox)},
    {"riotgames", CIVUE_ICON(simple_icons, riotgames)},
    {"leagueoflegends", CIVUE_ICON(simple_icons, leagueoflegends)},
  };

  for (size_t i = 0; i < countof(icons); i++) {
    if (strcmp(type, icons[i].type) == 0) {
      return icons[i].icon;
    }
  }

  return CIVUE_ICON(mingcute, earth_fill);
}

static void connections(App *app, const Profile *p, const Paint *c) {
  if (p->nconnections == 0) {
    return;
  }

  div(flex, flex_col, gap(2)) {
    heading(c, "Connections");

    for (size_t i = 0; i < p->nconnections; i++) {
      const ProfileConnection *pc = &p->connections[i];

      div(flex, items_center, gap(2), h(px(40)), px(3), rounded(lg),
          border(1), border_color(c->border)) {
        glyph(connection_icon(pc->type), 20, c->strong);
        text(pc->name, flex_1, min_w(0), truncate, font_weight(medium),
             text_color(c->strong));

        if (pc->verified) {
          glyph(CIVUE_ICON(mingcute, certificate_fill), 16, c->subtle);
        }
      }
    }
  }

  (void)app;
}

static void modal_roles(App *app, const User *u, const Profile *p,
                        const Paint *c) {
  const Guild *guild = store_guild(app->session->store, p->guild_id);
  const Member *m = guild ? store_member(guild, u->id) : nullptr;

  if (!m || guild->nroles == 0) {
    return;
  }

  const Role **roles = calloc(guild->nroles, sizeof *roles);
  size_t count = roles_of(guild, m, roles);

  if (count) {
    div(flex, flex_col, gap(2)) {
      heading(c, "Roles");

      div(flex, flex_wrap, gap(1)) {
        for (size_t i = 0; i < count; i++) {
          role_pill(roles[i], c);
        }
      }
    }
  }

  free(roles);
}

static civue_field_look note_look(const Look *k, const Paint *c) {
  civue_field_box box = {.text = c->text, .radius = 4};
  civue_field_box editing = box;

  editing.bg = c->field;

  return (civue_field_look){
    .pad = 4,
    .editing = editing,
    .idle = box,
    .hovered = editing,
    .placeholder = c->faint,
    .selection = k->selection,
  };
}

static void note(App *app, const User *u, const Profile *p, const Paint *c) {
  civue_ui *ui = civue_ui_current();
  civue_id key = id("modal-note");

  if (u->id == app->session->store->self_id) {
    return;
  }

  // what discord holds shows until the user writes in it.
  if (app->note_user != u->id && p->note_loaded) {
    free(app->profile_note);
    app->profile_note = p->note ? strdup(p->note) : nullptr;
    app->note_user = u->id;
  }

  div(flex, flex_col, gap(2)) {
    heading(c, "Note (only visible to you)");

    div(key(key), h(px(44)), mx(px(-4)), text_size(px(14)),
        line_height(px(18))) {
      civue_rect r = here();
      civue_field_response field =
        civue_text_area(ui, id_in(key, "field"), r, &app->profile_note,
                        "Click to add a note",
                        civue_node_text_style(ui), note_look(&app->look, c));

      // as discord's, it keeps 256 characters.
      if (field.changed && app->profile_note &&
          strlen(app->profile_note) > 256) {
        app->profile_note[256] = 0;
      }

      bool left = !field.focused && app->note_user == u->id &&
                  strcmp(app->profile_note ? app->profile_note : "",
                         p->note ? p->note : "") != 0;

      if (left && p->note_loaded) {
        session_set_note(app->session, u->id, app->profile_note);
      }
    }
  }
}

static void modal_card(App *app, const User *u, const Profile *p,
                       const Paint *c, float width) {
  const Look *k = &app->look;
  civue_ui *ui = civue_ui_current();
  float pad = width <= 380 ? 24 : 32;
  float avatar_size = width <= 350 ? 96 : MODAL_AVATAR;
  civue_color ground = c->themed ? c->start : k->surface;

  div(key(id("modal-card")), relative, flex, flex_col, flex_shrink_0,
      w(px(width)), h(full), rounded_t(px(16)), overflow_hidden, shadow(md),
      style(.bg = ground, .has_bg = true), text_color(c->text),
      text_size(px(14)), line_height(px(18))) {
    civue_rect r = here();

    if (c->themed) {
      gradient(r, 16, 0, c->start, c->end, r.y0 + MODAL_BANNER, r.y1);
    }

    civue_stroke(ui, r, k->border, 1, 16);

    div(flex, flex_col, flex_1, min_h(0), overflow_y_scroll,
        scrollbars(&app->thin_bars)) {
      div(relative, flex_shrink_0,
          h(px(MODAL_BANNER + avatar_size / 2 + 20))) {
        banner(app, u, p, c, p->guild_id, width, MODAL_BANNER, 16);

        div(absolute, left(px(pad)), top(px(MODAL_BANNER - avatar_size / 2))) {
          cut_avatar(app, u, p->guild_id, avatar_size, MODAL_CUTOUT, 24, 8,
                     ground, false);
        }

        if (has_custom_status(u)) {
          div(absolute, left(px(pad + avatar_size + 20)),
              top(px(MODAL_BANNER - 8)), right(px(16))) {
            status_bubble(app, u, c);
          }
        }
      }

      div(flex, flex_col, gap(6), mt(px(-4)), pt(1), px(px(pad)),
          pb(px(pad))) {
        names(app, id("modal-names"), u, p, c, p->guild_id, true, false);
        modal_buttons(app, u, p, c);

        const char *bio = bio_of(p, u);

        if (bio) {
          markup(app, id_at(id("modal-bio"), u->id), bio,
                 &(MarkupOptions){.guild_id = p->guild_id,
                                  .size = 14,
                                  .color = c->strong,
                                  .hint = width - pad * 2});
        }

        member_since(app, u, p, c);
        modal_roles(app, u, p, c);
        connections(app, p, c);
        note(app, u, p, c);
      }
    }
  }
}

static bool tab(App *app, civue_id id, const char *label, bool on,
                const Paint *c) {
  bool pressed = false;
  civue_color mark = c->themed ? c->strong : app->look.accent_text;

  div(key(id), relative, flex, flex_col, pb(4), cursor_pointer,
      text_size(px(14)), line_height(px(18)), font_weight(semibold),
      style(.text_color = on ? c->strong : c->subtle, .has_text_color = true),
      hover(text_color(c->strong))) {
    pressed = button().activated;
    text(label, whitespace_nowrap);

    if (on) {
      div(absolute, left(0), right(0), bottom(px(-1)), h(px(2)),
          rounded_t(px(2)), bg(mark)) {}
    }
  }

  return pressed;
}

static void empty_tab(const Paint *c, const char *line) {
  div(flex, flex_col, items_center, justify_center, flex_1, gap(2), py(16)) {
    icon(mingcute, group_fill, size(px(40)), text_color(c->faint));
    text(line, text_size(px(14)), line_height(px(18)), text_color(c->subtle));
  }
}

static void friends_tab(App *app, const Profile *p, const Paint *c) {
  Store *store = app->session->store;
  char doing[256];
  Snowflake chosen = 0;

  session_mutual_friends(app->session, p->user_id, p->guild_id);

  if (p->nmutual_friends == 0) {
    empty_tab(c, p->friends_loading ? "Loading..."
                                    : "You don't have any friends in common");
    return;
  }

  div(flex, flex_col, gap(3)) {
    for (size_t i = 0; i < p->nmutual_friends; i++) {
      const User *f = store_user(store, p->mutual_friends[i]);
      const char *line = nullptr;

      if (!f) {
        continue;
      }

      if (f->activity && *f->activity) {
        line = f->activity;
      }
      else if (f->nactivities) {
        line = activity_status_text(&f->activities[0], doing, sizeof doing);
      }

      div(key(id_at(id("modal-friend-row"), f->id)), flex, items_center,
          gap(px(10)), p(1), rounded(sm), hover(bg(c->hover)),
          cursor_pointer) {
        if (button().activated) {
          chosen = f->id;
        }

        avatar(app, f, 0, 40, true, app->look.chat);

        div(flex, flex_col, flex_1, min_w(0)) {
          text(store_user_name(f), truncate, text_size(px(16)),
               line_height(px(20)), font_weight(medium),
               text_color(c->strong));

          if (line) {
            text(line, truncate, text_size(px(12)), line_height(px(16)),
                 text_color(c->subtle));
          }
        }
      }
    }
  }

  if (chosen) {
    open_modal(app, chosen, 0, PROFILE_TAB_ACTIVITY);
  }
}

static void guilds_tab(App *app, const User *u, const Profile *p,
                       const Paint *c) {
  Store *store = app->session->store;
  Snowflake chosen = 0;

  if (p->nmutual_guilds == 0) {
    empty_tab(c, "You don't have any servers in common");
    return;
  }

  div(flex, flex_col, gap(3)) {
    for (size_t i = 0; i < p->nmutual_guilds; i++) {
      const ProfileGuild *mg = &p->mutual_guilds[i];
      const Guild *g = store_guild(store, mg->id);

      if (!g) {
        continue;
      }

      div(key(id_at(id("modal-guild-row"), g->id)), flex, items_center,
          gap(px(10)), p(1), rounded(sm), hover(bg(c->hover)),
          cursor_pointer) {
        if (button().activated) {
          chosen = g->id;
        }

        div(size(px(40)), flex_shrink_0, rounded(lg), overflow_hidden) {
          guild_icon(app, g, 40, false);
        }

        div(flex, flex_col, flex_1, min_w(0)) {
          text(g->name ? g->name : "", truncate, text_size(px(16)),
               line_height(px(20)), font_weight(medium),
               text_color(c->strong));

          if (mg->nick) {
            div(flex, items_center, gap(1), min_w(0)) {
              avatar(app, u, g->id, 16, false, app->look.chat);
              text(mg->nick, truncate, text_size(px(12)), line_height(px(16)),
                   font_weight(medium), text_color(c->subtle));
            }
          }
        }
      }
    }
  }

  if (chosen) {
    app->modal_user = 0;
    app_select_guild(app, chosen);
  }
}

static void activity_tab(App *app, const User *u, const Paint *c) {
  size_t shown = 0;

  div(flex, flex_col, gap(3)) {
    for (size_t i = 0; i < u->nactivities; i++) {
      if (u->activities[i].type != ACTIVITY_CUSTOM) {
        activity_card(app, id_at(id("modal-activity"), i), &u->activities[i],
                      c);
        shown++;
      }
    }
  }

  if (shown == 0) {
    div(flex, flex_col, items_center, justify_center, flex_1, gap(2),
        py(16)) {
      icon(mingcute, game_2_fill, size(px(40)), text_color(c->faint));
      text("Nothing to show here yet", text_size(px(14)), line_height(px(18)),
           text_color(c->subtle));
    }
  }
}

static void modal_tabs(App *app, const User *u, const Profile *p,
                       const Paint *c) {
  bool self = u->id == app->session->store->self_id;
  size_t friends = p->friends_loaded ? p->nmutual_friends
                   : p->mutual_friends_count > p->nmutual_friends
                     ? p->mutual_friends_count
                     : p->nmutual_friends;
  char friends_label[48], guilds_label[48];

  if (self && app->modal_tab != PROFILE_TAB_ACTIVITY) {
    app->modal_tab = PROFILE_TAB_ACTIVITY;
  }

  if (friends) {
    snprintf(friends_label, sizeof friends_label, "%zu Mutual Friend%s",
             friends, friends == 1 ? "" : "s");
  }
  else {
    snprintf(friends_label, sizeof friends_label, "No Mutual Friends");
  }

  if (p->nmutual_guilds) {
    snprintf(guilds_label, sizeof guilds_label, "%zu Mutual Server%s",
             p->nmutual_guilds, p->nmutual_guilds == 1 ? "" : "s");
  }
  else {
    snprintf(guilds_label, sizeof guilds_label, "No Mutual Servers");
  }

  div(flex, flex_col, flex_1, min_w(0), h(full), pl(8)) {
    div(flex, items_end, gap(6), flex_shrink_0, border_b(1),
        border_color(c->border)) {
      if (tab(app, id("modal-tab-activity"), "Activity",
              app->modal_tab == PROFILE_TAB_ACTIVITY ||
                app->modal_tab == PROFILE_TAB_ABOUT,
              c)) {
        app->modal_tab = PROFILE_TAB_ACTIVITY;
      }

      if (!self && !u->bot &&
          tab(app, id("modal-tab-friends"), friends_label,
              app->modal_tab == PROFILE_TAB_FRIENDS, c)) {
        app->modal_tab = PROFILE_TAB_FRIENDS;
      }

      if (!self && tab(app, id("modal-tab-guilds"), guilds_label,
                       app->modal_tab == PROFILE_TAB_GUILDS, c)) {
        app->modal_tab = PROFILE_TAB_GUILDS;
      }
    }

    div(key(id_at(id("modal-panel"), app->modal_tab)), flex, flex_col, flex_1,
        min_h(0), overflow_y_scroll, scrollbars(&app->thin_bars), pt(5),
        pb(4)) {
      switch (app->modal_tab) {
        case PROFILE_TAB_FRIENDS:
          friends_tab(app, p, c);
          break;
        case PROFILE_TAB_GUILDS:
          guilds_tab(app, u, p, c);
          break;
        default:
          activity_tab(app, u, c);
          break;
      }
    }
  }
}

// a close button, x in a circle.
static bool close_button(App *app, const Paint *c) {
  bool pressed = false;

  div(key(id("modal-close")), absolute, top(px(8)), right(px(8)), flex,
      items_center, justify_center, size(8), rounded(full), bg(c->chip),
      hover(bg(c->hover)), text_color(c->strong), cursor_pointer) {
    civue_button_state state = button();

    pressed = state.activated;
    icon(mingcute, close_fill, size(px(18)));
    hint(app, id("modal-close"), &state.response, "Close",
         CIVUE_TOOLTIP_BELOW);
  }

  return pressed;
}

void profile_modal(App *app) {
  if (app->modal_user == 0) {
    return;
  }

  civue_ui *ui = civue_ui_current();
  Session *s = app->session;
  const User *u = store_user(s->store, app->modal_user);

  if (u == nullptr) {
    app->modal_user = 0;
    return;
  }

  const Look *k = &app->look;
  const Profile *p =
    session_profile(s, app->modal_user, app->modal_guild, true);
  Paint c = paint_of(app, p);
  float vw = ui->size.x, vh = ui->size.y;
  bool wide = vw >= 929, tall = vh >= 785;
  float width = wide ? fminf(MODAL_WIDTH, vw - 96) : vw;
  float height = tall ? fminf(MODAL_HEIGHT, vh - 96) : vh;
  float edge = vw >= 1025 ? 48 : 32;
  float card = fminf(fmaxf(300, vw * .455f - 43.68f), 400);
  civue_id opens = id_at(id("modal-open"), app->profile_opens);
  float shown = civue_animate_from(ui, opens, 0, 1, .2f);
  // easeOutQuad, as discord's modals grow in.
  float eased = 1 - (1 - shown) * (1 - shown);
  float scale = .7f + .3f * eased;
  bool close = false;
  char url[CDN_URL_MAX] = "";

  height = fmaxf(height, fminf(MODAL_MIN_HEIGHT, vh));
  app->covers_buttons = true;

  if (banner_hash(p)) {
    banner_url(p, 1024, url);
  }

  overlay() {
    div(key(id("modal-backdrop")), left(0), top(0), w(px(vw)), h(px(vh)),
        flex, items_center, justify_center,
        bg(civue_alpha(k->scrim, eased))) {
      close = button().activated;

      div(key(id("modal")), relative, flex, flex_shrink_0,
          w(px(width * scale)), h(px(height * scale)),
          rounded(px(wide && tall ? 16 : 0)), overflow_hidden,
          style(.bg = civue_alpha(c.themed ? mix(c.primary, CIVUE_BLACK, .85f)
                                           : k->chat,
                                  eased),
                .has_bg = true),
          border(wide && tall ? 1 : 0), border_color(k->border_strong)) {
        civue_rect r = here();

        // a press on the modal is not one on what is behind it.
        button();

        if (*url) {
          civue_image_info info = civue_images_get(app->images, ui, url, 1024);

          if (info.texture) {
            float h = fminf(280, (float)civue_rect_h(r));
            civue_rect top = xywh(r.x0, r.y0, civue_rect_w(r), h);
            float iw = (float)info.width, ih = (float)info.height;
            float cover = fmaxf((float)civue_rect_w(top) / iw, h / ih);
            float u0 = .5f - (float)civue_rect_w(top) / (iw * cover) / 2;
            float v0 = .5f - h / (ih * cover) / 2;

            civue_image(ui, info.texture, top,
                        (float[4]){u0, v0, 1 - u0, 1 - v0}, .3f * eased, 0,
                        false);
            civue_fade_out(ui, top,
                           c.themed ? mix(c.primary, CIVUE_BLACK, .85f)
                                    : k->chat);
          }
        }

        if (eased > .99f) {
          div(flex, w(full), h(full), pt(px(edge)), pl(px(edge)),
              pr(px(edge - 16))) {
            modal_card(app, u, p, &c, card);
            modal_tabs(app, u, p, &c);
          }

          close |= close_button(app, &c);
        }
      }
    }
  }

  if (civue_key_pressed(ui, CIVUE_KEY_ESCAPE) && ui->focus == 0) {
    close = true;
  }

  if (close) {
    app->modal_user = 0;
  }
}
