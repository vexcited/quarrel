#include "components.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <civue/dsl.h>

#include "discord/cdn.h"
#include "discord/markdown.h"
#include "widgets.h"

constexpr float GUTTER = 72;
constexpr float MEDIA_MAX_WIDTH = 400;
constexpr float MEDIA_MAX_HEIGHT = 300;
constexpr int64_t GROUP_GAP_MS = 7 * 60 * 1000;

static civue_id message_key(const Message *m) {
  return id_at(id("message"), m->id ? m->id : m->nonce);
}

static bool starts_with(const char *s, const char *prefix) {
  return s && !strncmp(s, prefix, strlen(prefix));
}

static civue_vec2 fit_media(uint32_t width, uint32_t height, float max_width,
                            float max_height) {
  double w = width ? width : max_width;
  double h = height ? height : max_height;
  double scale = fmin(1, fmin(max_width / w, max_height / h));

  return point(fmax(round(w * scale), 16), fmax(round(h * scale), 16));
}

// an image of a message, opened in the browser when pressed.
static void media(App *app, civue_id id, const char *proxy_url, const char *url,
                  uint32_t width, uint32_t height, float max_width,
                  float max_height, bool spoiler) {
  const Look *k = &app->look;
  civue_ui *ui = civue_ui_current();
  civue_vec2 size = fit_media(width, height, max_width, max_height);
  char sized[CDN_URL_MAX];
  bool hidden = spoiler;

  for (uint32_t i = 0; hidden && i < app->revealed.len; i++) {
    hidden = app->revealed.v[i] != id;
  }

  cdn_media(sized, proxy_url ? proxy_url : url, (uint32_t)(size.x * ui->scale),
            (uint32_t)(size.y * ui->scale));

  div(key(id), flex, items_center, justify_center, flex_shrink_0, w(px(size.x)),
      h(px(size.y)), rounded(lg), bg(k->surface), cursor_pointer) {
    bool pressed = button().activated;

    if (hidden) {
      div(px(3), py(1), rounded(full), bg(rgba(0x000000b8))) {
        text("SPOILER", text_size(px(14)), font_weight(bold),
             text_color(CIVUE_WHITE));
      }

      if (pressed) {
        if (app->revealed.len == app->revealed.cap) {
          app->revealed.cap = app->revealed.cap ? app->revealed.cap * 2 : 16;
          app->revealed.v = realloc(app->revealed.v, app->revealed.cap *
                                                       sizeof *app->revealed.v);
        }

        app->revealed.v[app->revealed.len++] = id;
      }
    }
    else {
      image_fill(app, sized, 8);

      if (pressed && url) {
        civue_open_url(url);
      }
    }
  }
}

static void format_bytes(uint64_t size, char *out, size_t cap) {
  if (size < 1024) {
    snprintf(out, cap, "%llu bytes", (unsigned long long)size);
  }
  else if (size < 1024 * 1024) {
    snprintf(out, cap, "%.2f KB", (double)size / 1024);
  }
  else {
    snprintf(out, cap, "%.2f MB", (double)size / (1024 * 1024));
  }
}

static void file_card(App *app, civue_id id, const Attachment *a) {
  const Look *k = &app->look;
  char size[32];

  format_bytes(a->size, size, sizeof size);

  div(flex, items_center, gap(2), w(full), max_w(px(432)), p(4), rounded(lg),
      bg(k->surface), border(1), border_color(k->border)) {
    icon(lucide, file, size(8), flex_shrink_0, text_color(k->subtle));

    div(flex, flex_col, flex_1, min_w(0)) {
      div(key(id), text_color(k->link), cursor_pointer) {
        if (button().activated && a->url) {
          civue_open_url(a->url);
        }

        text(a->filename ? a->filename : "file", truncate);
      }

      text(size, text_size(px(12)), line_height(px(16)), text_color(k->muted));
    }
  }
}

static void attachments(App *app, const Message *m) {
  for (size_t i = 0; i < m->nattachments; i++) {
    const Attachment *a = &m->attachments[i];
    civue_id id = id_at(message_key(m), 0x100 + i);
    bool image = a->width && a->height &&
                 (starts_with(a->content_type, "image/") || !a->content_type);

    div(mt(1)) {
      if (image) {
        media(app, id, a->proxy_url, a->url, a->width, a->height,
              MEDIA_MAX_WIDTH, MEDIA_MAX_HEIGHT, a->spoiler);
      }
      else {
        file_card(app, id, a);
      }
    }
  }
}

static bool link_text(const Look *k, civue_id id, const char *label,
                      const char *url) {
  bool pressed = false;

  div(key(id), text_color(url ? k->link : k->strong),
      style(.cursor = url ? CIVUE_CURSOR_POINTER : CIVUE_CURSOR_DEFAULT,
            .has_cursor = true)) {
    pressed = button().activated && url;
    text(label, font_weight(semibold));
  }

  if (pressed) {
    civue_open_url(url);
  }

  return pressed;
}

static void embed_fields(App *app, civue_id id, const Embed *e,
                         Snowflake guild_id) {
  const Look *k = &app->look;

  for (size_t i = 0; i < e->nfields; i++) {
    const EmbedField *field = &e->fields[i];

    div(flex, flex_col, gap(px(2))) {
      text(field->name ? field->name : "", text_size(px(14)),
           line_height(px(18)), font_weight(semibold), text_color(k->strong));
      markup(app, id_at(id, 0x40 + i), field->value ? field->value : "",
             &(MarkupOptions){.guild_id = guild_id, .size = 14, .hint = 380});
    }
  }
}

static void embed(App *app, civue_id id, const Embed *e, Snowflake guild_id) {
  const Look *k = &app->look;
  bool bare = (starts_with(e->type, "image") || starts_with(e->type, "gifv")) &&
              !e->title && !e->description;
  const EmbedMedia *picture = e->image.url ? &e->image : &e->thumbnail;

  if (bare && picture->url) {
    media(app, id_at(id, 1), picture->proxy_url, picture->url, picture->width,
          picture->height, MEDIA_MAX_WIDTH, MEDIA_MAX_HEIGHT, false);
    return;
  }

  bool video = starts_with(e->type, "video");
  bool corner = e->thumbnail.url && !e->image.url && !video &&
                !starts_with(e->type, "article");
  char stamp[48];

  div(flex, self_start, max_w(px(432)), rounded(sm), bg(k->surface),
      border_l(4), border_color(e->has_color ? rgb(e->color) : rgb(0x393a41))) {
    div(flex, flex_col, flex_1, min_w(0), gap(2), pt(2), pr(4), pb(4), pl(3)) {
      if (e->provider_name) {
        text(e->provider_name, text_size(px(12)), line_height(px(16)),
             text_color(k->muted));
      }

      if (e->author_name) {
        div(text_size(px(14)), line_height(px(18))) {
          link_text(k, id_at(id, 2), e->author_name, e->author_url);
        }
      }

      if (e->title) {
        link_text(k, id_at(id, 3), e->title, e->url);
      }

      if (e->description) {
        markup(app, id_at(id, 4), e->description,
               &(MarkupOptions){.guild_id = guild_id, .size = 14, .hint = 380});
      }

      embed_fields(app, id, e, guild_id);

      if (!corner && picture->url) {
        media(app, id_at(id, 5), picture->proxy_url, picture->url,
              picture->width, picture->height, 400, 300, false);
      }

      if (e->footer_text || e->timestamp) {
        if (e->timestamp) {
          format_stamp(e->timestamp, stamp, sizeof stamp);
        }

        div(flex, gap(1), text_size(px(12)), line_height(px(16)),
            text_color(k->muted)) {
          if (e->footer_text) {
            text(e->footer_text, min_w(0), truncate);
          }

          if (e->footer_text && e->timestamp) {
            text("•");
          }

          if (e->timestamp) {
            text(stamp, whitespace_nowrap);
          }
        }
      }
    }

    if (corner) {
      div(pt(2), pr(4)) {
        media(app, id_at(id, 6), e->thumbnail.proxy_url, e->thumbnail.url,
              e->thumbnail.width, e->thumbnail.height, 80, 80, false);
      }
    }
  }
}

static void embeds(App *app, const Message *m, Snowflake guild_id) {
  if (m->flags & MESSAGE_FLAG_SUPPRESS_EMBEDS) {
    return;
  }

  for (size_t i = 0; i < m->nembeds; i++) {
    div(flex, flex_col, mt(1)) {
      embed(app, id_at(message_key(m), 0x1000 + i * 0x100), &m->embeds[i],
            guild_id);
    }
  }
}

static void stickers(App *app, const Message *m) {
  const Look *k = &app->look;

  for (size_t i = 0; i < m->nstickers; i++) {
    const Sticker *sticker = &m->stickers[i];
    char url[CDN_URL_MAX];

    cdn_sticker(url, sticker->id, sticker->format, 320);

    if (!*url) {
      text(sticker->name ? sticker->name : "Sticker", text_color(k->muted),
           italic);
      continue;
    }

    div(size(px(160)), mt(1)) {
      image_fill(app, url, 0);
    }
  }
}

static void reactions(App *app, const Message *m) {
  const Look *k = &app->look;

  if (!m->nreactions) {
    return;
  }

  div(flex, flex_wrap, gap(1), mt(1)) {
    for (size_t i = 0; i < m->nreactions; i++) {
      const Reaction *r = &m->reactions[i];
      char count[16];

      snprintf(count, sizeof count, "%u", r->count);

      div(key(id_at(message_key(m), 0x200 + i)), flex, items_center, gap(px(6)),
          h(7), px(px(7)), rounded(lg),
          bg(r->me ? civue_alpha(k->brand, .24f) : k->hover), border(1),
          border_color(r->me ? k->brand : CIVUE_CLEAR),
          hover(border_color(r->me ? k->brand : k->border_strong)),
          cursor_pointer) {
        if (button().activated) {
          session_react(app->session, m->channel_id, m->id, r->emoji_id,
                        r->name, !r->me);
        }

        if (r->emoji_id) {
          char url[CDN_URL_MAX];
          cdn_emoji(url, r->emoji_id, 48);

          div(size(4), flex_shrink_0) {
            image_fill(app, url, 0);
          }
        }
        else {
          text(r->name ? r->name : "", text_size(px(16)), line_height(px(20)));
        }

        text(count, text_size(px(14)), line_height(px(18)),
             font_weight(semibold),
             text_color(r->me ? k->mention_text : k->subtle));
      }
    }
  }
}

// the line over a reply: who and what it answers, tied to the avatar by a
// spine.
static void reply_line(App *app, const Message *m, Snowflake guild_id) {
  const Look *k = &app->look;
  Store *store = app->session->store;
  const Message *to = m->referenced;

  div(relative, flex, items_center, gap(1), h(px(18)), mb(1), text_size(px(14)),
      line_height(px(18)), text_color(k->subtle)) {
    div(absolute, left(px(-37)), top(px(8)), w(px(33)), h(px(14)), border_l(2),
        border_t(2), rounded_tl(px(6)), border_color(rgb(0x4f505a))) {}

    if (!to) {
      text("Original message was deleted", italic, text_color(k->muted));
      continue;
    }

    Author author = store_author(store, to);
    char *plain = nullptr;

    if (*to->content) {
      MdDoc *doc = md_parse(to->content, strlen(to->content),
                            &(MdOptions){.inline_only = true});
      plain = md_plain_text(doc);
      md_free(doc);
    }

    avatar(app, author.user, guild_id, 16, false, k->chat);
    text(author.name, flex_shrink_0, font_weight(medium), whitespace_nowrap,
         text_color(author.color ? rgb(author.color) : k->text));

    if (plain && *plain) {
      text(plain, flex_1, min_w(0), truncate);
    }
    else {
      text("Click to see attachment", italic, flex_1, min_w(0), truncate);
    }

    free(plain);
  }
}

static const char *system_text(const Message *m, char *out, size_t cap,
                               const char *other) {
  switch (m->type) {
    case MESSAGE_RECIPIENT_ADD:
      snprintf(out, cap, "added %s to the group.", other);
      break;
    case MESSAGE_RECIPIENT_REMOVE:
      snprintf(out, cap, "removed %s from the group.", other);
      break;
    case MESSAGE_CALL:
      return "started a call.";
    case MESSAGE_CHANNEL_NAME_CHANGE:
      snprintf(out, cap, "changed the channel name: %s", m->content);
      break;
    case MESSAGE_CHANNEL_ICON_CHANGE:
      return "changed the channel icon.";
    case MESSAGE_CHANNEL_PINNED_MESSAGE:
      return "pinned a message to this channel.";
    case MESSAGE_USER_JOIN:
      return "just showed up!";
    case MESSAGE_GUILD_BOOST:
    case MESSAGE_GUILD_BOOST_TIER_1:
    case MESSAGE_GUILD_BOOST_TIER_2:
    case MESSAGE_GUILD_BOOST_TIER_3:
      return "just boosted the server!";
    case MESSAGE_CHANNEL_FOLLOW_ADD:
      snprintf(out, cap, "has added %s to this channel.", m->content);
      break;
    case MESSAGE_THREAD_CREATED:
      snprintf(out, cap, "started a thread: %s", m->content);
      break;
    default:
      return *m->content ? m->content : "sent a message Quarrel cannot show.";
  }

  return out;
}

static void system_message(App *app, const Channel *c, const Message *m) {
  const Look *k = &app->look;
  Store *store = app->session->store;
  Author author = store_author(store, m);
  char line[512], stamp[48];
  const char *other = m->nmentions
                        ? store_display_name(store, c->guild_id, m->mentions[0])
                        : "someone";
  bool boost =
    m->type >= MESSAGE_GUILD_BOOST && m->type <= MESSAGE_GUILD_BOOST_TIER_3;

  format_stamp(m->timestamp, stamp, sizeof stamp);

  div(absolute, left(0), top(px(4)), w(px(GUTTER)), flex, justify_center) {
    if (m->type == MESSAGE_USER_JOIN || m->type == MESSAGE_RECIPIENT_ADD) {
      icon(lucide, arrow_right, size(px(18)), text_color(k->online));
    }
    else if (m->type == MESSAGE_CHANNEL_PINNED_MESSAGE) {
      icon(lucide, pin, size(px(18)), text_color(k->subtle));
    }
    else if (boost) {
      icon(lucide, gem, size(px(18)), text_color(rgb(0xff73fa)));
    }
    else {
      icon(lucide, info, size(px(18)), text_color(k->subtle));
    }
  }

  div(flex, flex_wrap, items_baseline, gap_x(1), text_color(k->subtle)) {
    text(author.name, font_weight(medium),
         text_color(author.color ? rgb(author.color) : k->strong));
    text(system_text(m, line, sizeof line, other), min_w(0));
    text(stamp, text_size(px(12)), text_color(k->faint), ml(1),
         whitespace_nowrap);
  }
}

static void header(App *app, const Channel *c, const Message *m) {
  const Look *k = &app->look;
  Author author = store_author(app->session->store, m);
  char stamp[48];
  bool tagged = (author.user && author.user->bot) || m->webhook_id;

  (void)c;
  format_stamp(m->timestamp, stamp, sizeof stamp);

  div(flex, items_baseline, gap(2), min_w(0)) {
    text(author.name, font_weight(medium), min_w(0), truncate,
         text_color(author.color ? rgb(author.color) : k->strong));

    if (tagged) {
      div(self_center, flex_shrink_0, px(px(5)), rounded(sm), bg(k->brand)) {
        text("APP", text_size(px(12)), line_height(px(16)),
             font_weight(semibold), text_color(CIVUE_WHITE));
      }
    }

    text(stamp, flex_shrink_0, text_size(px(12)), text_color(k->faint),
         whitespace_nowrap);
  }
}

static bool tool(App *app, civue_id id, civue_icon icon, const char *label,
                 civue_color color) {
  const Look *k = &app->look;
  bool pressed = false;

  div(key(id), flex, items_center, justify_center, size(8), rounded(md),
      text_color(color), hover(bg(k->hover), text_color(k->strong)),
      cursor_pointer) {
    civue_button_state state = button();
    pressed = state.activated;

    civue_node_icon(civue_ui_current(), icon,
                    &CIVUE_NODE_STYLE(.w = civue_px(20), .h = civue_px(20)));
    hint(app, id, &state.response, label, CIVUE_TOOLTIP_ABOVE);
  }

  return pressed;
}

static void quick_reaction(App *app, const Message *m, civue_id id,
                           const char *emoji) {
  const Look *k = &app->look;

  div(key(id), flex, items_center, justify_center, size(8), rounded(md),
      hover(bg(k->hover)), cursor_pointer) {
    if (button().activated) {
      bool mine = false;

      for (size_t i = 0; i < m->nreactions; i++) {
        const Reaction *r = &m->reactions[i];

        mine |= !r->emoji_id && r->name && !strcmp(r->name, emoji) && r->me;
      }

      session_react(app->session, m->channel_id, m->id, 0, emoji, !mine);
    }

    text(emoji, text_size(px(18)), line_height(px(22)));
  }
}

static void start_edit(App *app, const Message *m) {
  if (!app->editing) {
    app->kept_draft = app->draft;
    app->draft = nullptr;
  }

  free(app->draft);
  app->draft = strdup(m->content);
  app->editing = m->id;
  app->reply_to = 0;
  app->focus_composer = true;
}

static void toolbar(App *app, Channel *c, const Message *m) {
  const Look *k = &app->look;
  civue_ui *ui = civue_ui_current();
  Store *store = app->session->store;
  civue_id id = id_at(message_key(m), 0x300);
  bool own = m->author_id == store->self_id;
  bool removable =
    own || (c->guild_id && store_can(store, c, PERM_MANAGE_MESSAGES));
  // a first press arms the button, a second one deletes.
  bool *armed = civue_memory(ui, id, sizeof *armed, nullptr);

  div(absolute, right(4), top(px(-16)), flex, items_center, p(px(2)),
      rounded(lg), bg(k->surface), border(1), border_color(k->border),
      shadow(sm)) {
    app->toolbar_rect = civue_rect_inflate(here(), 4, 4);

    if (store_can(store, c, PERM_ADD_REACTIONS)) {
      quick_reaction(app, m, id_at(id, 1), "👍");
      quick_reaction(app, m, id_at(id, 2), "❤️");
      quick_reaction(app, m, id_at(id, 3), "😂");
    }

    if (tool(app, id_at(id, 4), CIVUE_ICON(lucide, reply), "Reply",
             k->subtle)) {
      app->reply_to = m->id;
      app->reply_ping = true;
      app->focus_composer = true;
    }

    if (own && !message_is_system(m) &&
        tool(app, id_at(id, 5), CIVUE_ICON(lucide, pencil), "Edit",
             k->subtle)) {
      start_edit(app, m);
    }

    if (removable && tool(app, id_at(id, 6), CIVUE_ICON(lucide, trash_2),
                          *armed ? "Press again to delete" : "Delete",
                          *armed ? k->critical_text : k->subtle)) {
      if (*armed) {
        session_delete(app->session, c->id, m->id);
      }

      *armed = !*armed;
    }
  }
}

static void failure(App *app, const Channel *c, const Message *m) {
  const Look *k = &app->look;
  civue_id id = id_at(message_key(m), 0x400);

  div(flex, items_center, gap(2), text_size(px(14)), line_height(px(18)),
      text_color(k->critical_text)) {
    text(m->error ? m->error : "Your message could not be delivered.");

    div(key(id_at(id, 1)), text_color(k->link), cursor_pointer) {
      if (button().activated) {
        session_retry(app->session, c->id, m->nonce);
      }

      text("Retry");
    }

    div(key(id_at(id, 2)), text_color(k->link), cursor_pointer) {
      if (button().activated) {
        session_discard(app->session, c->id, m->nonce);
      }

      text("Dismiss");
    }
  }
}

static void message(App *app, Channel *c, Message *m, bool head, float width) {
  const Look *k = &app->look;
  civue_ui *ui = civue_ui_current();
  Store *store = app->session->store;
  civue_id id = message_key(m);
  bool reply = m->reference_id && !m->forwarded && !message_is_system(m);
  bool hot = app->hover_message == (m->id ? m->id : m->nonce);
  bool editing = app->editing && app->editing == m->id;
  civue_color ground = m->mentions_me ? k->mentioned
                       : editing      ? civue_alpha(k->brand, .12f)
                                      : k->message_hover;
  civue_color ink = m->state == MESSAGE_PENDING  ? k->faint
                    : m->state == MESSAGE_FAILED ? k->critical_text
                                                 : k->text;

  div(key(id), relative, pl(px(GUTTER)), pr(12), py(px(2)),
      mt(px(head ? 17 : 0)),
      style(.bg = hot || m->mentions_me || editing ? ground : CIVUE_CLEAR)) {
    civue_rect r = here();
    civue_vec2 mouse = ui->input.mouse;

    if (ui->input.has_mouse && civue_rect_contains(r, mouse) &&
        civue_rect_contains(civue_clip_rect(ui), mouse) &&
        !civue_under_overlay(ui, mouse)) {
      app->hover_next = m->id ? m->id : m->nonce;
    }

    if (m->mentions_me) {
      div(absolute, left(0), top(0), bottom(0), w(px(2)),
          bg(k->mentioned_bar)) {}
    }

    if (message_is_system(m)) {
      system_message(app, c, m);
    }
    else {
      if (reply) {
        reply_line(app, m, c->guild_id);
      }

      if (head) {
        Author author = store_author(store, m);

        div(absolute, left(4), top(px(reply ? 26 : 2))) {
          avatar(app, author.user, c->guild_id, 40, false, k->chat);
        }

        header(app, c, m);
      }
      else if (hot) {
        char clock[16];
        format_clock(m->timestamp, clock, sizeof clock);

        div(absolute, left(0), top(px(2)), w(px(GUTTER - 16))) {
          text(clock, text_right, text_size(px(11)), line_height(px(22)),
               text_color(k->faint));
        }
      }

      if (*m->content) {
        markup(app, id_at(id, 1), m->content,
               &(MarkupOptions){
                 .guild_id = c->guild_id,
                 .color = ink,
                 .edited = m->edited_at != 0,
                 .hint = width - GUTTER - 48,
               });
      }

      attachments(app, m);
      embeds(app, m, c->guild_id);
      stickers(app, m);
      reactions(app, m);

      if (m->state == MESSAGE_FAILED) {
        failure(app, c, m);
      }
    }

    if (hot && m->state == MESSAGE_SENT) {
      toolbar(app, c, m);
    }
  }
}

static void divider(const Look *k, const char *label, bool fresh) {
  civue_color line = fresh ? k->danger : k->border;

  div(flex, items_center, gap(1), mt(6), mb(2), ml(4), mr(px(fresh ? 0 : 16)),
      h(px(0))) {
    div(flex_1, h(px), bg(line)) {}

    if (label) {
      text(label, px(1), text_size(px(12)), line_height(px(13)),
           font_weight(semibold), text_color(k->muted));
    }

    div(flex_1, h(px), bg(line)) {}

    if (fresh) {
      div(px(1), rounded_l(sm), bg(k->danger)) {
        text("NEW", text_size(px(10)), line_height(px(13)), font_weight(bold),
             text_color(CIVUE_WHITE));
      }
    }
  }
}

static void beginning(App *app, const Channel *c) {
  const Look *k = &app->look;
  Store *store = app->session->store;
  char name[128], title[192], lead[256];

  store_channel_name(store, c, name, sizeof name);

  if (channel_is_private(c)) {
    snprintf(title, sizeof title, "%s", name);
    snprintf(lead, sizeof lead,
             c->type == CHANNEL_DM
               ? "This is the beginning of your direct message history with "
                 "%s."
               : "Welcome to the beginning of the %s group.",
             name);
  }
  else {
    snprintf(title, sizeof title, "Welcome to #%s!", name);
    snprintf(lead, sizeof lead, "This is the start of the #%s channel.", name);
  }

  div(flex, flex_col, gap(2), px(4), pt(4), mb(2)) {
    if (c->type == CHANNEL_DM && c->nrecipients) {
      avatar(app, store_user(store, c->recipients[0]), 0, 80, false, k->chat);
    }
    else {
      div(flex, items_center, justify_center, size(px(68)), rounded(full),
          bg(k->surface_high), text_color(k->strong)) {
        if (channel_is_private(c)) {
          icon(lucide, users, size(10));
        }
        else {
          icon(lucide, hash, size(10));
        }
      }
    }

    text(title, text_size(px(32)), line_height(px(40)), font_weight(bold),
         text_color(k->strong));
    text(lead, text_color(k->subtle));
  }
}

static bool starts_group(const Message *m, const Message *before) {
  if (!before || message_is_system(m) || message_is_system(before)) {
    return true;
  }

  return m->author_id != before->author_id ||
         m->webhook_id != before->webhook_id ||
         (m->reference_id && !m->forwarded) ||
         m->timestamp - before->timestamp > GROUP_GAP_MS ||
         day_of(m->timestamp) != day_of(before->timestamp);
}

// where the list is, before its nodes: it follows its end while it sits
// there, and stays put when history arrives over what is read.
static void place_list(App *app, const Channel *c, civue_id list,
                       civue_id inner) {
  civue_ui *ui = civue_ui_current();
  float view = (float)last_size(list).y;
  float height = (float)last_size(inner).y;
  float most = fmaxf(height - view, 0);
  Snowflake last = c->nmessages ? c->messages[c->nmessages - 1]->id : 0;
  if (app->list_channel != c->id) {
    app->list_channel = c->id;
    app->stick = true;
    app->list_height = 0;
    app->hover_message = 0;
  }

  if (!ui->rerun) {
    bool up = ui->input.scroll.y > 0 || ui->input.down[0];

    // a slow scroll leaves the end by less than a point a frame, so the
    // list only follows again once the scroll has rested there.
    if (up) {
      app->stick = false;
      app->unstuck_at = ui->now;
    }
    else if (!app->stick && civue_scroll_y(ui, list) >= most - 1 &&
             ui->now - app->unstuck_at > .3) {
      app->stick = true;
    }
  }

  if (app->stick) {
    civue_set_scroll_y(ui, list, most);
  }
  else if (app->list_height > 0 && height != app->list_height &&
           last == app->list_last) {
    civue_set_scroll_y(ui, list, most - app->from_bottom);
  }

  float y = civue_scroll_y(ui, list);

  app->from_bottom = fmaxf(most - y, 0);
  app->list_height = height;
  app->list_last = last;

  if (y < 400 && c->has_older && !c->loading && c->history_loaded) {
    session_load_older(app->session, c->id);
  }

  if (app->stick && ui->window_focused) {
    session_ack(app->session, c->id);
  }
}

void message_list(App *app, Channel *c) {
  const Look *k = &app->look;
  Store *store = app->session->store;
  const Guild *guild = c->guild_id ? store_guild(store, c->guild_id) : nullptr;
  civue_ui *ui = civue_ui_current();
  civue_id list = id("messages"), inner = id("messages-inner");
  float width = (float)last_size(list).x;
  // the divider stays where it was when the channel was opened, the channel
  // itself being read at once.
  Snowflake *fresh =
    civue_memory(ui, id_in(list, "fresh"), 2 * sizeof *fresh, nullptr);

  if (fresh[0] != c->id) {
    fresh[0] = c->id;
    fresh[1] = store_last_acked(app->session->store, c->id);
  }

  place_list(app, c, list, inner);
  app->hover_next = 0;

  div(key(list), flex_1, min_h(0), overflow_y_scroll, scrollbars(&app->bars)) {
    div(key(inner), flex, flex_col, justify_end, min_h(full), pb(6)) {
      if (c->loading) {
        text("Loading messages...", px(4), py(3), text_color(k->muted));
      }
      else if (!c->has_older && c->history_loaded) {
        beginning(app, c);
      }

      const Message *before = nullptr;
      bool marked = false;

      for (size_t i = 0; i < c->nmessages; i++) {
        Message *m = c->messages[i];
        bool head = starts_group(m, before);
        bool unread = !marked && fresh[1] && m->id > fresh[1] &&
                      m->state == MESSAGE_SENT &&
                      m->author_id != app->session->store->self_id;

        if (!before || day_of(m->timestamp) != day_of(before->timestamp)) {
          char day[48];
          format_day(m->timestamp, day, sizeof day);
          divider(k, day, false);
        }

        if (unread) {
          marked = true;
          head = true;

          if (i > 0) {
            divider(k, nullptr, true);
          }
        }

        // a message is built again when what it reads changed. one drawing
        // an image is never kept, civue builds those every frame.
        Snowflake known = m->id ? m->id : m->nonce;
        uint64_t reads[] = {
          c->version,
          guild ? guild->members_version : 0,
          store->users_version,
          images_version(app->images),
          app->revealed.len,
          app->hover_message == known,
          app->editing == known,
          head,
          (uint64_t)width,
          (uint64_t)day_of((int64_t)store_now(store) * 1000),
        };

        memo(id_at(id("message-memo"), known),
             civue_hash(reads, sizeof reads)) {
          message(app, c, m, head, width);
        }

        before = m;
      }
    }
  }

  if (app->hover_next != app->hover_message) {
    civue_request_repaint(ui);
  }
}
