#include "components.h"

#include <math.h>
#include <stdcountof.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <civue/dsl.h>

#include "discord/cdn.h"
#include "discord/markdown.h"
#include "widgets.h"

// the measures of discord's stylesheets.
constexpr double SEARCH_REST = .35;
constexpr float THUMB = 72;
constexpr float CARD_GAP = 8;
constexpr float GALLERY_MIN = 320;
constexpr float GALLERY_GAP = 16;
constexpr float GOLDEN = 1.618f;
constexpr size_t CARD_TAGS = 3;
constexpr size_t BAR_TAGS = 8;
constexpr uint32_t POST_TAGS_MAX = 5;
// the end of the list asks for more this close.
constexpr float MORE_AT = 200;

static bool blank(const char *s) {
  for (; s && *s; s++) {
    if (*s != ' ' && *s != '\n' && *s != '\t') {
      return false;
    }
  }

  return true;
}

static void drop(char **text) {
  free(*text);
  *text = nullptr;
}

static bool media_channel(const Channel *forum) {
  return forum->type == CHANNEL_GUILD_MEDIA;
}

// what the page keeps is of one forum, and starts over in another.
static void enter_forum(App *app, const Channel *forum) {
  if (app->forum_id == forum->id) {
    return;
  }

  app->forum_id = forum->id;
  app->forum_tags.len = 0;
  app->forum_sort = -1;
  app->forum_layout = -1;
  app->forum_match_all = forum->tags_match_all;
  app->forum_typed_at = 0;
  app->post_open = false;
  app->post_tried = false;
  app->post_tags.len = 0;
  drop(&app->forum_search);
  drop(&app->post_title);
  drop(&app->post_body);
}

static ForumSort sort_of(const App *app, const Channel *forum) {
  return app->forum_sort >= 0 ? (ForumSort)app->forum_sort
                              : forum->default_sort;
}

// a media channel is always a gallery, a forum a list unless it says.
static bool gallery_of(const App *app, const Channel *forum) {
  ForumLayout layout = app->forum_layout >= 0 ? (ForumLayout)app->forum_layout
                                              : forum->default_layout;

  return media_channel(forum) || layout == FORUM_LAYOUT_GALLERY;
}

// the words searched for once the user stopped typing, null for none.
static const char *searched(App *app) {
  civue_ui *ui = civue_ui_current();

  if (blank(app->forum_search) || app->post_open) {
    return nullptr;
  }

  double rest = app->forum_typed_at + SEARCH_REST - ui->now;

  if (rest > 0) {
    civue_request_repaint_after(ui, rest);
    return nullptr;
  }

  return app->forum_search;
}

static void ask(App *app, const Channel *forum) {
  ForumQuery query = {
    .sort = sort_of(app, forum),
    .tags = app->forum_tags.v,
    .ntags = app->forum_tags.len,
    .match_all = app->forum_match_all,
    .query = (char *)searched(app),
  };

  session_forum_query(app->session, forum->id, &query);
}

static bool may_post(App *app, const Channel *forum) {
  return store_can(app->session->store, forum, PERM_SEND_MESSAGES);
}

static bool may_moderate(App *app, const Channel *forum) {
  return store_can(app->session->store, forum, PERM_MANAGE_THREADS);
}

/* ========================================================================== */
/* which posts show, in which order.                                          */
/* ========================================================================== */

static bool wears(const Channel *post, Snowflake tag) {
  for (size_t i = 0; i < post->napplied_tags; i++) {
    if (post->applied_tags[i] == tag) {
      return true;
    }
  }

  return false;
}

static bool tags_match(const App *app, const Channel *post) {
  if (!app->forum_tags.len) {
    return true;
  }

  for (uint32_t i = 0; i < app->forum_tags.len; i++) {
    bool worn = wears(post, app->forum_tags.v[i]);

    if (worn && !app->forum_match_all) {
      return true;
    }

    if (!worn && app->forum_match_all) {
      return false;
    }
  }

  return app->forum_match_all;
}

static Snowflake activity_of(const Channel *c) {
  return c->last_message_id > c->id ? c->last_message_id : c->id;
}

static bool pinned(const Channel *c) {
  return (c->flags & CHANNEL_FLAG_PINNED) != 0;
}

// pinned first, then the active posts, then the archived ones under their
// divider.
static int rank(const Channel *c) {
  return pinned(c) ? 0 : c->thread_archived ? 2 : 1;
}

static int by_activity(const void *a, const void *b) {
  const Channel *x = *(Channel *const *)a, *y = *(Channel *const *)b;

  if (rank(x) != rank(y)) {
    return rank(x) - rank(y);
  }

  Snowflake ax = activity_of(x), ay = activity_of(y);

  return ax > ay ? -1 : ax < ay;
}

static int by_creation(const void *a, const void *b) {
  const Channel *x = *(Channel *const *)a, *y = *(Channel *const *)b;

  if (rank(x) != rank(y)) {
    return rank(x) - rank(y);
  }

  return x->id > y->id ? -1 : x->id < y->id;
}

// the posts to list, from malloc. a search lists what discord found in its
// order, else every post known that wears the tags asked for.
static Channel **collect(App *app, const Channel *forum, bool found,
                         size_t *count) {
  Store *store = app->session->store;
  const SessionForum *f = &app->session->forum;
  size_t known = 0;
  Channel *const *posts = store_forum_posts(store, forum, &known);
  size_t cap = found ? f->nposts : known;
  Channel **out = calloc(cap + 1, sizeof *out);

  *count = 0;

  for (size_t i = 0; i < cap; i++) {
    Channel *c = found ? store_channel(store, f->posts[i]) : posts[i];

    if (c && store_can(store, c, PERM_VIEW_CHANNEL) &&
        (found || tags_match(app, c))) {
      out[(*count)++] = c;
    }
  }

  if (!found) {
    qsort(out, *count, sizeof *out,
          sort_of(app, forum) == FORUM_SORT_CREATION ? by_creation
                                                     : by_activity);
  }

  return out;
}

/* ========================================================================== */
/* pieces of a post.                                                          */
/* ========================================================================== */

// "1m ago" up to an hour, then hours, days, ">30d ago", and the date past a
// year, as the client writes them.
static void format_since(int64_t ms, char *out, size_t cap) {
  int64_t seconds = (int64_t)time(nullptr) - ms / 1000;

  if (seconds < 3600) {
    snprintf(out, cap, "%lldm ago",
             (long long)(seconds < 60 ? 1 : seconds / 60));
  }
  else if (seconds < 86400) {
    snprintf(out, cap, "%lldh ago", (long long)(seconds / 3600));
  }
  else if (seconds <= 86400 * 30) {
    snprintf(out, cap, "%lldd ago", (long long)(seconds / 86400));
  }
  else if (seconds < 86400 * 365) {
    snprintf(out, cap, ">30d ago");
  }
  else {
    format_day(ms, out, cap);
  }
}

void tag_pill(App *app, const ForumTag *tag, bool small) {
  const Look *k = &app->look;

  div(flex, items_center, gap(1), flex_shrink_0, h(px(small ? 24 : 32)),
      px(px(small ? 8 : 12)), rounded(full), bg(k->hover), border(1),
      border_color(k->border)) {
    if (tag->emoji_id || tag->emoji_name) {
      emoji_node(app, tag->emoji_id, tag->emoji_name, small ? 12 : 14);
    }

    text(tag->name ? tag->name : "", text_size(px(small ? 12 : 14)),
         line_height(px(small ? 16 : 18)), font_weight(semibold),
         whitespace_nowrap, text_color(k->text));
  }
}

void post_tags(App *app, const Channel *post, size_t most) {
  const Look *k = &app->look;
  const Channel *forum = store_channel(app->session->store, post->parent_id);
  size_t shown = 0, left = 0;

  if (!post->napplied_tags || !forum) {
    return;
  }

  div(flex, flex_wrap, items_center, gap(2)) {
    for (size_t i = 0; i < post->napplied_tags; i++) {
      const ForumTag *tag = store_forum_tag(forum, post->applied_tags[i]);

      if (!tag) {
        continue;
      }

      if (shown < most) {
        tag_pill(app, tag, true);
        shown++;
      }
      else {
        left++;
      }
    }

    if (left) {
      char more[16];

      snprintf(more, sizeof more, "+%zu", left);
      text(more, text_size(px(12)), line_height(px(16)), font_weight(semibold),
           text_color(k->subtle));
    }
  }
}

// the first image of a post's first message, "" for none.
static void thumbnail_of(const Message *m, uint32_t pixels, char *out) {
  out[0] = 0;

  for (size_t i = 0; m && i < m->nattachments; i++) {
    const Attachment *a = &m->attachments[i];
    bool image = a->content_type && (!strncmp(a->content_type, "image/", 6) ||
                                     !strncmp(a->content_type, "video/", 6));

    if (!a->spoiler && (image || a->width)) {
      cdn_media(out, a->proxy_url ? a->proxy_url : a->url, pixels, pixels);
      return;
    }
  }

  for (size_t i = 0; m && i < m->nembeds; i++) {
    const EmbedMedia *media = m->embeds[i].image.url ? &m->embeds[i].image
                                                     : &m->embeds[i].thumbnail;

    if (media->url) {
      cdn_media(out, media->proxy_url ? media->proxy_url : media->url, pixels,
                pixels);
      return;
    }
  }
}

// the first message in a line: who wrote it and what it says.
static void preview(App *app, const Channel *post, bool lit) {
  const Look *k = &app->look;
  Store *store = app->session->store;
  const Message *m = post->starter;

  div(flex, items_center, min_w(0), h(px(18)), text_size(px(14)),
      line_height(px(18)), font_weight(medium), text_color(k->subtle)) {
    if (!m) {
      text(post->starter_known ? "Original message was deleted" : "",
           truncate, min_w(0), text_color(k->muted));
      continue;
    }

    const Relationship *r = store_relationship(store, m->author_id);

    if (r && r->type == RELATIONSHIP_BLOCKED) {
      text("Post contains a blocked message", truncate, min_w(0));
      continue;
    }

    if (r && r->ignored) {
      text("Post contains an ignored message", truncate, min_w(0));
      continue;
    }

    Author author = store_author(store, m);
    char *plain = nullptr;
    char name[160];

    if (*m->content) {
      MdDoc *doc = md_parse(m->content, strlen(m->content),
                            &(MdOptions){.inline_only = true});

      plain = md_plain_text(doc);
      md_free(doc);
    }

    snprintf(name, sizeof name, "%s:", author.name);
    text(name, flex_shrink_0, max_w(1_2), truncate, font_weight(semibold),
         whitespace_nowrap, text_color(lit ? k->strong : k->muted));
    text(plain && *plain       ? plain
         : m->nattachments ? "Click to see attachment"
                           : "Click to see post",
         flex_1, min_w(0), ml(1), truncate);

    free(plain);
  }
}

// the reaction the list shows: the one the post was reacted with the
// most, else the forum's own emoji, without a count.
static const Reaction *top_reaction(const Message *m) {
  const Reaction *most = nullptr;

  for (size_t i = 0; m && i < m->nreactions; i++) {
    if (!most || m->reactions[i].count > most->count) {
      most = &m->reactions[i];
    }
  }

  return most;
}

static void reaction_button(App *app, const Channel *forum,
                            const Channel *post) {
  const Look *k = &app->look;
  const Message *m = post->starter;
  const Reaction *r = top_reaction(m);
  Snowflake emoji_id = r ? r->emoji_id : forum->default_reaction_id;
  const char *name = r ? r->name : forum->default_reaction_name;
  bool me = r && r->me;
  char count[16] = "";

  if (!m || (!emoji_id && !name)) {
    return;
  }

  if (r) {
    snprintf(count, sizeof count, "%u", r->count);
  }

  div(key(id_at(id("post-reaction"), post->id)), flex, items_center, gap(1),
      flex_shrink_0, h(px(20)), px(px(6)), rounded(md),
      bg(me ? civue_alpha(k->primary, .24f) : k->hover), border(1),
      border_color(me ? k->primary : CIVUE_CLEAR),
      hover(border_color(me ? k->primary : k->border_strong)), cursor_pointer) {
    if (button().activated) {
      session_react(app->session, post->id, m->id, emoji_id, name, !me);
    }

    emoji_node(app, emoji_id, name, 14);

    if (*count) {
      text(count, text_size(px(14)), line_height(px(18)), font_weight(semibold),
           text_color(me ? k->mention_text : k->subtle));
    }
  }
}

// new to the user: active, never read, came since the forum was last
// looked at and after the user joined the guild.
static bool post_new(App *app, const Channel *post, Snowflake seen) {
  Store *store = app->session->store;
  const Guild *g = store_guild(store, post->guild_id);
  int64_t joined = g ? g->joined_at : 0;

  return !post->thread_archived && post->id > seen &&
         !store_has_read_state(store, post->id) &&
         post->owner_id != store->self_id &&
         snowflake_time_ms(post->id) > joined;
}

// the post has messages the user did not read.
static bool post_unread(App *app, const Channel *post) {
  Unread unread = store_channel_unread(app->session->store, post);

  return !post->thread_archived && unread.unread && !unread.muted;
}

static void pin_badge(const Look *k) {
  div(flex, items_center, justify_center, size(px(22)), flex_shrink_0,
      rounded(full), bg(k->primary), text_color(k->on_primary)) {
    icon(mingcute, pin_2_fill, size(px(14)));
  }
}

static void new_badge(const Look *k) {
  div(flex, items_center, flex_shrink_0, h(px(16)), px(px(4)), rounded(sm),
      bg(k->primary)) {
    text("NEW", text_size(px(10)), line_height(px(12)), font_weight(bold),
         text_color(k->on_primary));
  }
}

// how many messages came since the user last read the post, "1+" while
// discord did not say, up to "25+".
static void format_news(const Channel *post, char *out, size_t cap) {
  uint32_t news = post->new_known ? post->new_messages : 0;

  if (news > post->message_count && post->message_count) {
    news = post->message_count;
  }

  if (!news) {
    snprintf(out, cap, "(1+ New)");
  }
  else if (news >= 25) {
    snprintf(out, cap, "(25+ New)");
  }
  else {
    snprintf(out, cap, "(%u New)", news);
  }
}

static void format_count(const Channel *post, char *out, size_t cap) {
  // threads older than this one counted no further than fifty.
  constexpr Snowflake COUNTED = 992549565104128000;

  if (post->id < COUNTED && post->message_count >= 50) {
    snprintf(out, cap, "50+");
  }
  else if (post->message_count >= 100000) {
    snprintf(out, cap, "100k+");
  }
  else {
    snprintf(out, cap, "%u", post->message_count);
  }
}

static void footer(App *app, const Channel *forum, const Channel *post) {
  const Look *k = &app->look;
  Store *store = app->session->store;
  Unread unread = store_channel_unread(store, post);
  bool by_date = sort_of(app, forum) == FORUM_SORT_CREATION;
  int64_t at = by_date ? snowflake_time_ms(post->id)
                       : snowflake_time_ms(activity_of(post));
  char messages[24], since[48], news[24];

  format_count(post, messages, sizeof messages);
  format_since(at, since, sizeof since);

  div(flex, items_center, gap(2), min_w(0), h(px(20)), text_size(px(14)),
      line_height(px(18)), font_weight(medium), text_color(k->faint)) {
    reaction_button(app, forum, post);

    div(flex, items_center, gap(1), flex_shrink_0) {
      icon(mingcute, message_2_fill, size(px(14)));
      text(messages);

      if (post_unread(app, post) && store_has_read_state(store, post->id)) {
        format_news(post, news, sizeof news);
        text(news, font_weight(semibold), text_color(k->link));
      }
    }

    mention_badge(k, unread.mentions);
    text("•", flex_shrink_0);
    text(since, min_w(0), truncate);
  }
}

static void open_post(App *app, const Channel *post) {
  app->popover = POPOVER_NONE;
  app_select_channel(app, post->id);
}

static void list_card(App *app, const Channel *forum, const Channel *post,
                      Snowflake seen) {
  const Look *k = &app->look;
  civue_ui *ui = civue_ui_current();
  bool fresh = post_new(app, post, seen);
  bool lit = post_unread(app, post);
  char thumb[CDN_URL_MAX];

  thumbnail_of(post->starter, (uint32_t)(THUMB * ui->scale), thumb);

  div(key(id_at(id("post"), post->id)), flex, flex_shrink_0, py(3), px(4),
      rounded(xl), bg(k->panel), border(1), border_color(k->border),
      hover(bg(k->surface_high)), cursor_pointer) {
    if (button().activated) {
      open_post(app, post);
    }

    div(flex, flex_col, flex_1, min_w(0)) {
      if (post->napplied_tags || pinned(post)) {
        div(flex, items_center, gap(2), h(6), mb(2), min_w(0),
            overflow_hidden) {
          if (pinned(post)) {
            pin_badge(k);
          }

          post_tags(app, post, CARD_TAGS);
        }
      }

      div(flex, items_center, gap(2), min_w(0), pb(px(6))) {
        text(post->name ? post->name : "", min_w(0), text_size(px(20)),
             line_height(px(24)), font_weight(semibold),
             text_color(lit || fresh ? k->strong : k->muted));

        if (fresh) {
          new_badge(k);
        }

        if (post->thread_locked) {
          icon(mingcute, lock_fill, size(px(16)), flex_shrink_0,
               text_color(k->faint));
        }
      }

      div(flex, flex_col, pb(2)) {
        preview(app, post, lit || fresh);
      }

      footer(app, forum, post);
    }

    if (*thumb) {
      div(size(px(THUMB)), flex_shrink_0, self_center, ml(3), rounded(lg),
          bg(k->surface), overflow_hidden) {
        image_cover(app, thumb, 8);
      }
    }
  }
}

static void gallery_card(App *app, const Channel *forum, const Channel *post,
                         Snowflake seen, float width) {
  const Look *k = &app->look;
  civue_ui *ui = civue_ui_current();
  bool fresh = post_new(app, post, seen);
  bool lit = post_unread(app, post);
  float media = floorf(fmaxf(width - 2, 0) / GOLDEN);
  char thumb[CDN_URL_MAX];

  thumbnail_of(post->starter, (uint32_t)(width * ui->scale), thumb);

  div(key(id_at(id("post"), post->id)), flex, flex_col, min_w(0),
      rounded(xl), bg(k->panel), border(1), border_color(k->border),
      overflow_hidden, hover(bg(k->surface_high)), cursor_pointer) {
    if (button().activated) {
      open_post(app, post);
    }

    div(relative, flex, items_center, justify_center, w(full),
        h(px(media > 0 ? media : 180)), flex_shrink_0, bg(k->surface),
        text_color(k->faint)) {
      if (!*thumb || !image_cover(app, thumb, 0)) {
        icon(mingcute, message_square_text_fill, size(10));
      }

      if (pinned(post)) {
        div(absolute, left(px(8)), top(px(8))) {
          pin_badge(k);
        }
      }
    }

    div(flex, flex_col, gap(2), py(3), px(4), min_w(0)) {
      div(flex, items_center, gap(2), min_w(0)) {
        text(post->name ? post->name : "", min_w(0), truncate,
             text_size(px(16)), line_height(px(20)), font_weight(semibold),
             text_color(lit || fresh ? k->strong : k->muted));

        if (fresh) {
          new_badge(k);
        }
      }

      if (post->napplied_tags) {
        div(h(6), overflow_hidden) {
          post_tags(app, post, CARD_TAGS);
        }
      }

      footer(app, forum, post);
    }
  }
}

static void older_divider(const Look *k) {
  div(flex, items_center, gap(2), pt(4), pb(2)) {
    text("Older Posts", flex_shrink_0, text_size(px(12)), line_height(px(16)),
         font_weight(semibold), text_color(k->muted));
    div(flex_1, h(px), bg(k->border)) {}
  }
}

/* ========================================================================== */
/* menus.                                                                     */
/* ========================================================================== */

static void dismiss(App *app, civue_rect card, civue_rect anchor) {
  civue_ui *ui = civue_ui_current();
  bool outside = ui->input.pressed[0] && ui->input.has_mouse &&
                 !civue_rect_contains(card, ui->input.mouse) &&
                 !civue_rect_contains(anchor, ui->input.mouse);

  if (outside || civue_key_pressed(ui, CIVUE_KEY_ESCAPE)) {
    app->popover = POPOVER_NONE;
  }
}

static float card_left(civue_rect anchor, float width) {
  civue_rect room = overlay_room();
  float left = fminf((float)anchor.x0, (float)room.x1 - width);

  return fmaxf(left, (float)room.x0);
}

static void menu_title(const Look *k, const char *title) {
  text(title, px(2), pt(2), pb(1), text_size(px(12)), line_height(px(16)),
       font_weight(semibold), text_color(k->faint));
}

static void menu_line(const Look *k) {
  div(h(px), my(1), mx(2), flex_shrink_0, bg(k->border)) {}
}

// a row of a menu, ticked when `on`. true when pressed.
static bool choice(App *app, civue_id key_id, const char *label, bool on) {
  const Look *k = &app->look;
  bool picked = false;

  div(key(key_id), flex, items_center, gap(2), flex_shrink_0, h(8), px(2),
      rounded(md), text_size(px(14)), line_height(px(18)), font_weight(medium),
      text_color(on ? k->strong : k->text), hover(bg(k->hover)),
      cursor_pointer) {
    picked = button().activated;

    text(label, flex_1, min_w(0), truncate);

    if (on) {
      icon(mingcute, check_fill, size(4), text_color(k->primary));
    }
  }

  return picked;
}

static void view_menu(App *app, const Channel *forum, civue_rect anchor) {
  if (app->popover != POPOVER_FORUM_VIEW) {
    return;
  }

  const Look *k = &app->look;
  ForumSort sort = sort_of(app, forum);
  bool gallery = gallery_of(app, forum);
  civue_rect card = {};

  overlay() {
    div(key(id("forum-view-card")), flex, flex_col, w(px(180)),
        left(px(card_left(anchor, 180))), top(px(anchor.y1 + 6)), p(2),
        rounded(xl), bg(k->popover), border(1), border_color(k->border_strong),
        shadow(lg)) {
      card = here();

      menu_title(k, "Sort by");

      if (choice(app, id("sort-activity"), "Recently Active",
                 sort == FORUM_SORT_ACTIVITY)) {
        app->forum_sort = FORUM_SORT_ACTIVITY;
      }

      if (choice(app, id("sort-creation"), "Date Posted",
                 sort == FORUM_SORT_CREATION)) {
        app->forum_sort = FORUM_SORT_CREATION;
      }

      if (!media_channel(forum)) {
        menu_line(k);
        menu_title(k, "View as");

        if (choice(app, id("view-list"), "List", !gallery)) {
          app->forum_layout = FORUM_LAYOUT_LIST;
        }

        if (choice(app, id("view-gallery"), "Gallery", gallery)) {
          app->forum_layout = FORUM_LAYOUT_GALLERY;
        }
      }

      if (forum->ntags) {
        menu_line(k);
        menu_title(k, "Tag Matching");

        if (choice(app, id("match-some"), "Match Some",
                   !app->forum_match_all)) {
          app->forum_match_all = false;
        }

        if (choice(app, id("match-all"), "Match All", app->forum_match_all)) {
          app->forum_match_all = true;
        }
      }

      menu_line(k);

      if (choice(app, id("view-reset"), "Reset to default", false)) {
        app->forum_sort = -1;
        app->forum_layout = -1;
        app->forum_match_all = forum->tags_match_all;
      }
    }
  }

  dismiss(app, card, anchor);
}

// a tag that filters the list, lit while it does.
static void filter_tag(App *app, const ForumTag *tag) {
  const Look *k = &app->look;
  bool on = ids_has(&app->forum_tags, tag->id);

  div(key(id_at(id("filter-tag"), tag->id)), flex, items_center, gap(1),
      flex_shrink_0, h(8), px(3), rounded(full), border(1),
      border_color(on ? k->primary : k->border),
      style(.bg = on ? civue_alpha(k->primary, .15f) : k->hover),
      hover(border_color(on ? k->primary : k->border_strong)), cursor_pointer) {
    if (button().activated) {
      ids_toggle(&app->forum_tags, tag->id);
    }

    if (tag->emoji_id || tag->emoji_name) {
      emoji_node(app, tag->emoji_id, tag->emoji_name, 14);
    }

    text(tag->name ? tag->name : "", text_size(px(14)), line_height(px(18)),
         font_weight(semibold), whitespace_nowrap,
         text_color(on ? k->strong : k->text));
  }
}

static void tags_menu(App *app, const Channel *forum, civue_rect anchor) {
  if (app->popover != POPOVER_FORUM_TAGS) {
    return;
  }

  const Look *k = &app->look;
  civue_rect room = overlay_room();
  float left = fmaxf((float)room.x0, (float)anchor.x1 - 336);
  civue_rect card = {};

  overlay() {
    div(key(id("forum-tags-card")), flex, flex_col, gap(4), w(px(336)),
        left(px(left)), top(px(anchor.y1 + 6)), p(4), rounded(xl),
        bg(k->popover), border(1), border_color(k->border_strong), shadow(lg)) {
      card = here();

      text("Select Tags", text_size(px(16)), line_height(px(20)),
           font_weight(semibold), text_color(k->strong));

      div(flex, flex_wrap, gap(2)) {
        for (size_t i = 0; i < forum->ntags; i++) {
          filter_tag(app, &forum->tags[i]);
        }
      }

      div(key(id("forum-tags-clear")), self_start, text_size(px(14)),
          line_height(px(18)), font_weight(medium),
          text_color(app->forum_tags.len ? k->link : k->faint),
          cursor_pointer) {
        if (button().activated) {
          app->forum_tags.len = 0;
        }

        text("Clear all");
      }
    }
  }

  dismiss(app, card, anchor);
}

static void filter_bar(App *app, const Channel *forum) {
  const Look *k = &app->look;
  civue_rect view = {}, all = {};
  bool open = app->popover == POPOVER_FORUM_VIEW;

  div(flex, items_center, flex_shrink_0, min_w(0), pt(4), pb(1)) {
    div(key(id("forum-view")), flex, items_center, gap(2), flex_shrink_0,
        h(8), pl(2), pr(3), rounded(full), border(1),
        border_color(k->border), style(.bg = open ? k->hover : CIVUE_CLEAR),
        hover(bg(k->hover)), text_size(px(14)), line_height(px(18)),
        font_weight(semibold), text_color(k->text), cursor_pointer) {
      view = here();

      if (button().activated) {
        app_toggle(app, POPOVER_FORUM_VIEW);
      }

      icon(mingcute, transfer_vertical_fill, size(4));
      text(media_channel(forum) ? "Sort" : "Sort & View", whitespace_nowrap);
      icon(mingcute, down_fill, size(4));
    }

    if (forum->ntags) {
      div(w(px(2)), h(6), mx(4), flex_shrink_0, bg(k->border_strong)) {}

      div(flex, items_center, gap(2), flex_1, min_w(0), h(8),
          overflow_hidden) {
        for (size_t i = 0; i < forum->ntags && i < BAR_TAGS; i++) {
          filter_tag(app, &forum->tags[i]);
        }
      }

      bool lit = app->forum_tags.len > 0;
      char label[24];

      snprintf(label, sizeof label, lit ? "%u" : "All", app->forum_tags.len);

      div(key(id("forum-tags")), flex, items_center, gap(1), flex_shrink_0,
          h(8), ml(2), px(3), rounded(full), border(1),
          border_color(lit ? k->primary : k->border),
          style(.bg = lit ? civue_alpha(k->primary, .15f) : CIVUE_CLEAR),
          hover(bg(k->hover)), text_size(px(14)), line_height(px(18)),
          font_weight(semibold), text_color(k->text), cursor_pointer) {
        all = here();

        if (button().activated) {
          app_toggle(app, POPOVER_FORUM_TAGS);
        }

        text(label);
        icon(mingcute, down_fill, size(4));
      }
    }
  }

  view_menu(app, forum, view);
  tags_menu(app, forum, all);
}

/* ========================================================================== */
/* the header card: the search, which is the title of a post being written.   */
/* ========================================================================== */

static bool *guidelines_open(const Channel *forum) {
  return civue_memory(civue_ui_current(), id_at(id("guidelines"), forum->id),
                      sizeof(bool), nullptr);
}

static void guidelines(App *app, const Channel *forum) {
  const Look *k = &app->look;
  bool *open = guidelines_open(forum);

  if (!*open || blank(forum->topic)) {
    return;
  }

  div(flex, flex_col, flex_shrink_0, mt(4), p(3), rounded(xl), bg(k->panel),
      border(1), border_color(k->border)) {
    div(flex, items_center, gap(2)) {
      icon(mingcute, book_6_fill, size(5), text_color(k->subtle));
      text("Post Guidelines", flex_1, text_size(px(20)), line_height(px(24)),
           font_weight(semibold), text_color(k->strong));

      if (icon_button(app, id("guidelines-close"), CIVUE_ICON(mingcute, close_fill), 28,
                      18, "Close")) {
        *open = false;
      }
    }

    div(key(id_at(id("guidelines-text"), forum->id)), mt(3), p(4),
        rounded(lg), bg(k->hover), max_h(px(220)), overflow_y_scroll,
        scrollbars(&app->thin_bars)) {
      markup(app, id_at(id("guidelines-markup"), forum->id), forum->topic,
             &(MarkupOptions){.guild_id = forum->guild_id, .size = 14});
    }
  }
}

static void close_post(App *app) {
  app->post_open = false;
  app->post_tried = false;
  app->post_tags.len = 0;
  drop(&app->post_title);
  drop(&app->post_body);
}

static void start_post(App *app, const Channel *forum) {
  bool *open = guidelines_open(forum);
  bool *shown = civue_memory(civue_ui_current(),
                             id_at(id("guidelines-shown"), forum->id),
                             sizeof(bool), nullptr);

  app->post_open = true;
  app->post_focus = true;
  app->post_tried = false;
  app->post_tags.len = 0;
  app->popover = POPOVER_NONE;

  // what was searched for is the title, the forum's template the message.
  free(app->post_title);
  app->post_title = blank(app->forum_search) ? nullptr
                                             : strdup(app->forum_search);
  free(app->post_body);
  app->post_body = forum->template ? strdup(forum->template) : nullptr;

  // the guidelines open the first time a post is written here.
  if (!*shown) {
    *shown = true;
    *open = true;
  }
}

static void field_button(App *app, civue_id key_id, civue_icon glyph_icon,
                         const char *label, bool *pressed) {
  const Look *k = &app->look;

  div(key(key_id), flex, items_center, gap(2), flex_shrink_0, h(8), px(3),
      rounded(lg), bg(k->primary), hover(bg(k->primary_hover)),
      active(bg(k->primary_active)), text_size(px(14)), line_height(px(18)),
      font_weight(semibold), text_color(k->on_primary), cursor_pointer) {
    *pressed |= button().activated;
    glyph(glyph_icon, 16, k->on_primary);
    text(label, whitespace_nowrap);
  }
}

// the tags a post being written may wear, the moderated ones for who may
// put them on.
static void compose_tags(App *app, const Channel *forum) {
  const Look *k = &app->look;
  bool full = app->post_tags.len >= POST_TAGS_MAX;
  bool moderator = may_moderate(app, forum);

  div(flex, flex_wrap, items_center, gap(2), px(4), py(3)) {
    icon(mingcute, tag_fill, size(4), text_color(k->faint));

    for (size_t i = 0; i < forum->ntags; i++) {
      const ForumTag *tag = &forum->tags[i];
      bool on = ids_has(&app->post_tags, tag->id);
      bool allowed = on || !full;

      if (tag->moderated && !moderator) {
        continue;
      }

      div(key(id_at(id("compose-tag"), tag->id)), flex, items_center, gap(1),
          flex_shrink_0, h(6), px(2), rounded(full), border(1),
          border_color(on ? k->primary : k->border),
          style(.bg = on ? civue_alpha(k->primary, .15f) : k->hover,
                .cursor = allowed ? CIVUE_CURSOR_POINTER : CIVUE_CURSOR_DEFAULT,
                .has_cursor = true),
          hover(border_color(allowed ? k->border_strong : k->border))) {
        if (button().activated && allowed) {
          ids_toggle(&app->post_tags, tag->id);
        }

        if (tag->emoji_id || tag->emoji_name) {
          emoji_node(app, tag->emoji_id, tag->emoji_name, 12);
        }

        text(tag->name ? tag->name : "", text_size(px(12)),
             line_height(px(16)), font_weight(semibold), whitespace_nowrap,
             text_color(!allowed ? k->faint : on ? k->strong : k->text));
      }
    }
  }
}

// what the post lacks, null when it can go.
static const char *post_missing(App *app, const Channel *forum) {
  bool needs_tag = (forum->flags & CHANNEL_FLAG_REQUIRE_TAG) && forum->ntags;

  if (blank(app->post_title)) {
    return "A title is required";
  }

  if (blank(app->post_body)) {
    return "A message is required";
  }

  if (needs_tag && !app->post_tags.len) {
    return "A tag is required to create a post in this channel";
  }

  return nullptr;
}

static void send_post(App *app, const Channel *forum) {
  app->post_tried = true;

  if (!post_missing(app, forum) && !app->session->post_creating) {
    session_create_post(app->session, forum->id, app->post_title,
                        app->post_body, app->post_tags.v, app->post_tags.len);
  }
}

static void header_card(App *app, const Channel *forum) {
  const Look *k = &app->look;
  civue_ui *ui = civue_ui_current();
  Session *session = app->session;
  bool allowed = may_post(app, forum);
  bool writing = app->post_open;
  civue_id field_id = id("forum-field");
  bool start = false, send = false;
  const char *placeholder = writing   ? "Title"
                            : allowed ? "Search or create a post..."
                                      : "Search";

  // the post asked for exists: it opens.
  if (session->post_created &&
      store_channel(session->store, session->post_created)) {
    Snowflake id = session->post_created;

    session->post_created = 0;
    close_post(app);
    drop(&app->forum_search);
    app_select_channel(app, id);
    return;
  }

  if (writing && app->popover == POPOVER_NONE &&
      civue_key_pressed(ui, CIVUE_KEY_ESCAPE)) {
    close_post(app);
    writing = false;
  }

  div(flex, flex_col, flex_shrink_0, rounded_b(xl), bg(k->input), border_b(1),
      border_color(k->border_strong)) {
    div(flex, items_center, gap(2), h(px(56)), pl(4), pr(3)) {
      if (!writing) {
        icon(mingcute, search_fill, size(5), flex_shrink_0, text_color(k->faint));
      }

      div(key(field_id), flex_1, min_w(0), h(10),
          text_size(px(writing ? 20 : 16)), line_height(px(24)),
          style(.font_weight = writing ? 600 : 500)) {
        civue_field_look look = {
          .pad = 0,
          .editing = {.text = k->strong},
          .idle = {.text = k->strong},
          .hovered = {.text = k->strong},
          .placeholder = k->faint,
          .selection = k->selection,
        };
        char **value = writing ? &app->post_title : &app->forum_search;
        civue_field_response field = text_field(value, placeholder, look);

        if (field.changed && !writing) {
          app->forum_typed_at = ui->now;
        }

        // shift and enter writes a post titled after the search.
        if (field.submitted && !writing && allowed &&
            (ui->input.mods & CIVUE_MOD_SHIFT)) {
          start = true;
        }
      }

      if (writing && app->post_focus) {
        ui->focus = field_id;

        if (civue_field_caret_to_end(ui, field_id)) {
          app->post_focus = false;
        }
      }

      if (!writing && !blank(app->forum_search) &&
          icon_button(app, id("forum-search-clear"), CIVUE_ICON(mingcute, close_fill), 28,
                      16, "Clear")) {
        drop(&app->forum_search);
      }

      if (!blank(forum->topic)) {
        bool *open = guidelines_open(forum);

        if (icon_button(app, id("guidelines-toggle"),
                        CIVUE_ICON(mingcute, book_6_fill), 32, 20,
                        "Post Guidelines")) {
          *open = !*open;
        }
      }

      if (allowed && !writing) {
        field_button(app, id("new-post"), CIVUE_ICON(mingcute, message_2_fill),
                     "New Post", &start);
      }
      else if (writing) {
        field_button(app, id("post-send"), CIVUE_ICON(mingcute, message_2_fill),
                     session->post_creating ? "Posting..." : "Post", &send);
      }
    }

    if (writing) {
      div(key(id("post-body")), h(px(120)), mx(4), mb(1), text_size(px(16)),
          line_height(px(22))) {
        civue_field_look look = field_look(k);
        civue_text_style style = civue_style(16, 400, 22, 0);

        look.pad = 12;
        civue_text_area(ui, id("post-body"), here(), &app->post_body,
                        "Enter a message...", style, look);
      }

      if (forum->ntags) {
        compose_tags(app, forum);
      }

      div(h(px), mx(4), bg(k->border)) {}

      div(flex, items_center, gap(2), h(10), px(4), text_size(px(14)),
          line_height(px(18))) {
        const char *missing = post_missing(app, forum);
        const char *error = session->post_error ? session->post_error
                            : app->post_tried   ? missing
                                                : nullptr;

        text(error ? error : "Escape to cancel", flex_1, min_w(0), truncate,
             text_color(error ? k->critical_text : k->faint));

        div(key(id("post-cancel")), flex, items_center, h(8), px(3),
            rounded(lg), font_weight(medium), text_color(k->text),
            hover(bg(k->hover)), cursor_pointer) {
          if (button().activated) {
            close_post(app);
          }

          text("Cancel");
        }
      }
    }
  }

  if (start) {
    start_post(app, forum);
  }

  if (send) {
    send_post(app, forum);
  }
}

// under the header while words are searched: what was found.
static void search_line(App *app, size_t found) {
  const Look *k = &app->look;
  const SessionForum *f = &app->session->forum;
  char line[160];
  bool resting = !blank(app->forum_search) && !f->query.query;

  if (blank(app->forum_search) || app->post_open) {
    return;
  }

  if (f->loading || resting) {
    snprintf(line, sizeof line, "Searching...");
  }
  else if (!found) {
    snprintf(line, sizeof line, "No matching posts");
  }
  else {
    snprintf(line, sizeof line, "%zu %s with \"%s\"", found,
             found == 1 ? "post" : "posts", f->query.query);
  }

  div(flex, items_center, gap(2), h(8), mt(2), text_size(px(14)),
      line_height(px(18)), text_color(k->subtle)) {
    text(line, min_w(0), truncate, font_weight(medium));

    div(key(id("search-clear")), flex_shrink_0, text_color(k->link),
        font_weight(medium), cursor_pointer) {
      if (button().activated) {
        drop(&app->forum_search);
      }

      text("Clear");
    }

    div(flex_1) {}

    if (may_post(app, store_channel(app->session->store, app->forum_id))) {
      text("SHIFT + ENTER to create a new post", flex_shrink_0,
           text_size(px(12)), line_height(px(16)), font_weight(semibold),
           text_color(k->faint));
    }
  }
}

/* ========================================================================== */
/* the page.                                                                  */
/* ========================================================================== */

static void forum_header(App *app, const Channel *forum) {
  const Look *k = &app->look;

  div(flex, items_center, gap(2), flex_shrink_0, h(px(HEADER_HEIGHT)), px(4),
      border_b(1), border_color(k->border)) {
    window_handle(id("forum-header"));

    if (media_channel(forum)) {
      icon(mingcute, pic_fill, size(5), flex_shrink_0, text_color(k->faint));
    }
    else {
      icon(mingcute, messages_square_fill, size(5), flex_shrink_0,
           text_color(k->faint));
    }

    text(forum->name ? forum->name : "", flex_1, min_w(0), truncate,
         font_weight(semibold), text_color(k->strong));

    header_end(app);
  }
}

static void empty_state(App *app, const Channel *forum) {
  const Look *k = &app->look;
  uint32_t tags = app->forum_tags.len;
  bool allowed = may_post(app, forum);
  char title[160], lead[192];

  if (tags) {
    snprintf(title, sizeof title, "There are no posts with %s yet!",
             tags == 1 ? "this tag" : "these tags");
    snprintf(lead, sizeof lead, "Be the first to start a conversation with %s",
             tags == 1 ? "this tag" : "these tags");
  }
  else {
    snprintf(title, sizeof title, "Be the first to start the conversation!");
    snprintf(lead, sizeof lead, "What do you want to post about in #%s?",
             forum->name ? forum->name : "");
  }

  div(flex, flex_col, items_center, gap(2), mt(4), p(4), py(10), rounded(xl),
      bg(k->panel), text_center) {
    div(flex, items_center, justify_center, size(16), rounded(full),
        bg(k->surface_high), text_color(k->subtle)) {
      icon(mingcute, messages_square_fill, size(8));
    }

    text(title, mt(2), text_size(px(20)), line_height(px(24)),
         font_weight(semibold), text_color(k->strong));
    text(lead, text_size(px(14)), line_height(px(18)), text_color(k->subtle));

    if (allowed && !app->post_open) {
      div(key(id("empty-post")), mt(2), flex, items_center, h(9), px(4),
          rounded(lg), bg(k->primary), hover(bg(k->primary_hover)),
          text_size(px(14)), line_height(px(18)), font_weight(semibold),
          text_color(k->on_primary), cursor_pointer) {
        if (button().activated) {
          start_post(app, forum);
        }

        text("Create a post");
      }
    }
  }
}

static void posts(App *app, Channel *forum, Channel **list, size_t count,
                  bool found, Snowflake seen, float width) {
  const Look *k = &app->look;
  const SessionForum *f = &app->session->forum;
  bool mine = f->channel_id == forum->id;

  if (gallery_of(app, forum)) {
    uint32_t cols = (uint32_t)fmaxf(
      1, floorf((width + GALLERY_GAP) / (GALLERY_MIN + GALLERY_GAP)));
    float card = (width - GALLERY_GAP * (float)(cols - 1)) / (float)cols;

    div(grid, grid_cols((uint16_t)cols), gap(px(GALLERY_GAP)), mt(4)) {
      for (size_t i = 0; i < count; i++) {
        gallery_card(app, forum, list[i], seen, card);
      }
    }
  }
  else {
    div(flex, flex_col, gap(px(CARD_GAP)), mt(4)) {
      for (size_t i = 0; i < count; i++) {
        bool older = !found && list[i]->thread_archived && !pinned(list[i]) &&
                     (i == 0 || !list[i - 1]->thread_archived);

        if (older) {
          older_divider(k);
        }

        list_card(app, forum, list[i], seen);
      }
    }
  }

  if (mine && f->loading) {
    div(flex, flex_col, gap(px(CARD_GAP)), mt(px(CARD_GAP))) {
      float shades[] = {.33f, 1, .66f};

      for (size_t i = 0; i < countof(shades); i++) {
        div(h(px(102)), rounded(xl), bg(civue_alpha(k->panel, shades[i]))) {}
      }
    }
  }
  else if (mine && f->error) {
    text(f->error, py(4), text_center, text_size(px(14)), line_height(px(18)),
         text_color(k->critical_text));
  }
  else if (!count && !found) {
    empty_state(app, forum);
  }
}

void forum_page(App *app, Channel *forum) {
  civue_ui *ui = civue_ui_current();
  Session *session = app->session;
  civue_id list_id = id_at(id("forum-posts"), forum->id);
  civue_id inner_id = id_in(list_id, "inner");
  float width = (float)last_size(inner_id).x - 32;
  size_t count = 0;
  // the posts new to the user stay marked while the forum is open, the
  // forum itself being read at once.
  Snowflake *seen = civue_memory(ui, id_in(list_id, "seen"), 2 * sizeof *seen,
                                 nullptr);

  enter_forum(app, forum);
  ask(app, forum);

  if (seen[0] != forum->id) {
    seen[0] = forum->id;
    seen[1] = store_last_acked(session->store, forum->id);
  }

  if (ui->window_focused) {
    session_ack(session, forum->id);
  }

  const SessionForum *f = &session->forum;
  bool found = f->channel_id == forum->id && f->query.query;
  Channel **list = collect(app, forum, found, &count);
  Snowflake *missing = calloc(count + 1, sizeof *missing);
  size_t nmissing = 0;

  for (size_t i = 0; i < count; i++) {
    if (!list[i]->starter_known) {
      missing[nmissing++] = list[i]->id;
    }
  }

  session_fetch_starters(session, forum->id, missing, nmissing);
  free(missing);

  float view = (float)last_size(list_id).y;
  float height = (float)last_size(inner_id).y;

  if (height > 0 && civue_scroll_y(ui, list_id) > height - view - MORE_AT) {
    session_forum_more(session);
  }

  div(flex, flex_col, flex_1, min_w(0), h(full)) {
    forum_header(app, forum);
    header_card(app, forum);

    div(key(list_id), flex_1, min_h(0), overflow_y_scroll,
        scrollbars(&app->bars)) {
      div(key(inner_id), flex, flex_col, w(full), px(4), pb(6)) {
        search_line(app, count);
        guidelines(app, forum);
        filter_bar(app, forum);
        posts(app, forum, list, count, found, seen[1], width);
      }
    }
  }

  free(list);
}

/* ========================================================================== */
/* what can be done with a post.                                              */
/* ========================================================================== */

// the forum's other pinned post, which a new pin takes the place of.
static Channel *pinned_post(App *app, const Channel *forum,
                            const Channel *except) {
  size_t count = 0;
  Channel *const *list = store_forum_posts(app->session->store, forum, &count);

  for (size_t i = 0; i < count; i++) {
    if (list[i] != except && pinned(list[i])) {
      return list[i];
    }
  }

  return nullptr;
}

void post_menu(App *app, Channel *post, civue_rect anchor) {
  Store *store = app->session->store;
  const Channel *forum = store_channel(store, post->parent_id);

  if (app->popover != POPOVER_POST_MENU || !forum) {
    return;
  }

  const Look *k = &app->look;
  bool moderator = may_moderate(app, forum);
  bool owner = post->owner_id == store->self_id;
  civue_rect room = overlay_room();
  float left = fmaxf((float)room.x0, (float)anchor.x1 - 260);
  civue_rect card = {};

  overlay() {
    div(key(id("post-menu-card")), flex, flex_col, w(px(260)), left(px(left)),
        top(px(anchor.y1 + 6)), p(2), rounded(xl), bg(k->popover), border(1),
        border_color(k->border_strong), shadow(lg)) {
      card = here();

      if (forum->ntags && (owner || moderator)) {
        menu_title(k, "Tags");

        for (size_t i = 0; i < forum->ntags; i++) {
          const ForumTag *tag = &forum->tags[i];
          bool on = wears(post, tag->id);
          bool full = post->napplied_tags >= POST_TAGS_MAX;

          if (tag->moderated && !moderator) {
            continue;
          }

          if (choice(app, id_at(id("post-menu-tag"), tag->id),
                     tag->name ? tag->name : "", on) &&
              (on || !full)) {
            Snowflake tags[POST_TAGS_MAX + 1];
            size_t n = 0;

            for (size_t j = 0; j < post->napplied_tags && n < POST_TAGS_MAX;
                 j++) {
              if (post->applied_tags[j] != tag->id) {
                tags[n++] = post->applied_tags[j];
              }
            }

            if (!on) {
              tags[n++] = tag->id;
            }

            session_set_post_tags(app->session, post->id, tags, n);
          }
        }

        menu_line(k);
      }

      if (moderator) {
        if (menu_row(app, id("post-pin"), CIVUE_ICON(mingcute, pin_2_fill),
                     pinned(post) ? "Unpin Post" : "Pin Post", k->text)) {
          Channel *other = pinned(post) ? nullptr
                                        : pinned_post(app, forum, post);

          if (other) {
            session_update_post(app->session, other->id, false,
                                other->thread_archived, other->thread_locked);
          }

          session_update_post(app->session, post->id, !pinned(post), false,
                              post->thread_locked);
          app->popover = POPOVER_NONE;
        }

        if (menu_row(app, id("post-lock"), CIVUE_ICON(mingcute, lock_fill),
                     post->thread_locked ? "Unlock Post" : "Lock Post",
                     k->text)) {
          session_update_post(app->session, post->id, pinned(post),
                              post->thread_archived, !post->thread_locked);
          app->popover = POPOVER_NONE;
        }
      }

      if (owner || moderator) {
        bool closed = post->thread_archived;

        if (menu_row(app, id("post-close"),
                     closed ? CIVUE_ICON(mingcute, unarchive_fill)
                            : CIVUE_ICON(mingcute, archive_fill),
                     closed ? "Reopen Post" : "Close Post", k->text)) {
          session_update_post(app->session, post->id, pinned(post) && closed,
                              !closed, post->thread_locked);
          app->popover = POPOVER_NONE;
        }
      }

      if (menu_row(app, id("post-follow"),
                   post->thread_joined ? CIVUE_ICON(mingcute, notification_off_fill)
                                       : CIVUE_ICON(mingcute, notification_fill),
                   post->thread_joined ? "Unfollow Post" : "Follow Post",
                   k->text)) {
        session_follow(app->session, post->id, !post->thread_joined);
        app->popover = POPOVER_NONE;
      }
    }
  }

  dismiss(app, card, anchor);
}
