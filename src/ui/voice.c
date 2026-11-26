#include "components.h"

#include <stdio.h>

#include <civue/dsl.h>

#include "alerts.h"
#include "forms.h"
#include "voice/devices.h"
#include "voice/voice.h"
#include "widgets.h"

constexpr float MEMBER_AVATAR = 20;
constexpr float CARD_WIDTH = 232;
// with the line that names another account.
constexpr float CARD_HEIGHT = 270;
constexpr float CARD_GAP = 12;
constexpr float CARD_AVATAR = 80;
constexpr size_t CARDS_MOST = 4;

static bool speaks(const Session *s, const Channel *c, Snowflake user_id) {
  return s->voice && s->voice_channel_id == c->id &&
         voice_speaking(s->voice, user_id);
}

static void voice_member(App *app, const Channel *c, const VoiceState *v) {
  const Look *k = &app->look;
  Store *store = app->session->store;
  const User *user = store_user(store, v->user_id);
  bool speaking = speaks(app->session, c, v->user_id);

  div(key(id_at(id("voice-member"), v->user_id)), flex, items_center, gap(2),
      h(7), ml(9), mr(2), px(2), rounded(lg), hover(bg(k->hover)),
      cursor_pointer) {
    profile_trigger(app, v->user_id, c->guild_id, PROFILE_RIGHT);

    div(relative, flex_shrink_0) {
      avatar(app, user, c->guild_id, MEMBER_AVATAR, false, k->frame);

      if (speaking) {
        civue_rect r = here();

        stroke(xywh(r.x0 - 2, r.y0 - 2, MEMBER_AVATAR + 4, MEMBER_AVATAR + 4),
               k->positive, 2, MEMBER_AVATAR / 2 + 2);
      }
    }

    text(store_display_name(store, c->guild_id, v->user_id), flex_1, min_w(0),
         truncate, text_size(px(14)), line_height(px(18)),
         text_color(speaking ? k->strong : k->subtle));

    if (v->streaming) {
      glyph(CIVUE_ICON(lucide, monitor), 14, k->faint);
    }

    if (v->deafened) {
      glyph(CIVUE_ICON(lucide, headphone_off), 14, k->faint);
    }
    else if (v->muted) {
      glyph(CIVUE_ICON(lucide, mic_off), 14, k->faint);
    }
  }
}

void voice_members(App *app, const Channel *c) {
  const Store *store = app->session->store;

  for (size_t i = 0; i < store->nvoice_states; i++) {
    if (store->voice_states[i].channel_id == c->id) {
      voice_member(app, c, &store->voice_states[i]);
    }
  }
}

// a button of the panel, lit in `color` while `on`.
static bool panel_button(App *app, civue_id id, civue_icon icon,
                         const char *label, bool on, civue_color color) {
  const Look *k = &app->look;
  bool pressed = false;

  div(key(id), flex, items_center, justify_center, flex_1, h(8), rounded(lg),
      style(.bg = on ? k->hover : CIVUE_CLEAR), hover(bg(k->hover)),
      active(bg(k->pressed)), cursor_pointer) {
    civue_button_state state = button();

    pressed = state.activated;
    glyph(icon, 18, on ? color : k->subtle);
    hint(app, id, &state.response, label, CIVUE_TOOLTIP_ABOVE);
  }

  return pressed;
}

// how the call goes in a few words, and the color to say it in.
static const char *status_line(App *app, civue_color *color) {
  const Look *k = &app->look;
  const Session *s = app->session;

  *color = k->idle;

  if (s->voice_error) {
    *color = k->critical_text;
    return s->voice_error;
  }

  if (!s->voice) {
    return "Joining...";
  }

  switch (voice_status(s->voice)) {
    case VOICE_CONNECTED:
      *color = k->positive;
      return "Voice Connected";
    case VOICE_RESUMING:
      return "Reconnecting...";
    case VOICE_FAILED:
      *color = k->critical_text;
      return voice_error(s->voice);
    default:
      return "Connecting...";
  }
}

void voice_panel(App *app) {
  const Look *k = &app->look;
  Session *s = app->session;
  Store *store = s->store;

  if (!s->voice_channel_id) {
    return;
  }

  const Channel *c = store_channel(store, s->voice_channel_id);
  const Guild *guild = store_guild(store, s->voice_guild_id);
  bool live = s->voice && voice_status(s->voice) == VOICE_CONNECTED;
  civue_color color;
  const char *status = status_line(app, &color);
  char where[192], name[128] = "", detail[64] = "";

  if (c) {
    store_channel_name(store, c, name, sizeof name);
  }

  snprintf(where, sizeof where, "%s%s%s", name,
           guild && guild->name ? " / " : "",
           guild && guild->name ? guild->name : "");

  if (live && voice_latency(s->voice) >= 0) {
    snprintf(detail, sizeof detail, "%.0f ms", voice_latency(s->voice) * 1000);
  }

  div(key(id("voice-panel")), flex, flex_col, gap(2), flex_shrink_0, mx(2),
      mb(2), p(2), rounded(xl), bg(k->surface), border(1),
      border_color(k->border), text_size(px(14)), line_height(px(18))) {
    div(flex, items_center, gap(2), min_w(0), px(1)) {
      div(flex, flex_col, flex_1, min_w(0)) {
        text(status, truncate, font_weight(semibold), text_color(color));
        text(where, truncate, text_size(px(12)), line_height(px(16)),
             text_color(k->faint));
      }

      if (*detail) {
        text(detail, flex_shrink_0, text_size(px(12)), line_height(px(16)),
             text_color(k->faint));
      }

      // the call's sound is encrypted from one end to the other.
      if (live && voice_encrypted(s->voice)) {
        glyph(CIVUE_ICON(lucide, lock), 14, k->positive);
      }
    }

    if (live && voice_no_microphone(s->voice)) {
      text("Quarrel may not use the microphone, see the system's settings.",
           px(1), text_size(px(12)), line_height(px(16)),
           text_color(k->critical_text));
    }
    else if (live && voice_microphone_starting(s->voice)) {
      text("Starting the microphone...", px(1), text_size(px(12)),
           line_height(px(16)), text_color(k->faint));
    }

    div(flex, items_center, gap(1)) {
      bool muted = s->voice_mute || s->voice_deaf;
      civue_icon microphone =
        muted ? CIVUE_ICON(lucide, mic_off) : CIVUE_ICON(lucide, mic);
      civue_icon sound = s->voice_deaf ? CIVUE_ICON(lucide, headphone_off)
                                       : CIVUE_ICON(lucide, headphones);

      if (panel_button(app, id("voice-mute"), microphone,
                       muted ? "Unmute" : "Mute", muted, k->critical_text)) {
        session_voice_mute(s, !muted);
      }

      if (panel_button(app, id("voice-deafen"), sound,
                       s->voice_deaf ? "Undeafen" : "Deafen", s->voice_deaf,
                       k->critical_text)) {
        session_voice_deafen(s, !s->voice_deaf);
      }

      if (panel_button(app, id("voice-leave"), CIVUE_ICON(lucide, phone_off),
                       "Disconnect", true, k->critical_text)) {
        session_voice_leave(s);
      }
    }
  }
}

void voice_join(App *app, Snowflake channel_id) {
  Session *s = app->session;

  if (s->voice_channel_id != 0 && s->voice_channel_id != channel_id &&
      devices_option(VOICE_SWITCH_WARNING)) {
    app->voice_switch = channel_id;
    return;
  }

  session_voice_join(s, channel_id);
}

void voice_switch_dialog(App *app) {
  const Look *k = &app->look;
  civue_ui *ui = civue_ui_current();
  Session *s = app->session;
  const Channel *to = store_channel(s->store, app->voice_switch);
  const Channel *from = store_channel(s->store, s->voice_channel_id);
  bool close = false, join = false;
  char line[384], left[128], joined[128];

  if (app->voice_switch == 0) {
    return;
  }

  // the call ended, or the channel went, while it asked.
  if (to == nullptr || from == nullptr) {
    app->voice_switch = 0;
    return;
  }

  // a private channel is named after who is in it.
  snprintf(line, sizeof line, "You are in a call %s %s. Joining %s leaves it.",
           channel_is_private(from) ? "with" : "in",
           store_channel_name(s->store, from, left, sizeof left),
           store_channel_name(s->store, to, joined, sizeof joined));

  overlay() {
    div(key(id("voice-switch-backdrop")), left(0), top(0), w(px(ui->size.x)),
        h(px(ui->size.y)), flex, items_center, justify_center,
        bg(k->scrim)) {
      close = button().activated;

      div(key(id("voice-switch")), flex, flex_col, gap(4),
          w(px(dialog_width())), p(6), rounded(px(DIALOG_RADIUS)),
          bg(k->dialog), border(1), border_color(k->border), shadow(lg),
          text_color(k->text), text_size(px(14)), line_height(px(20))) {
        // a press on the dialog is not one on what is behind it.
        button();

        dialog_header(k, "Switch voice channel?", line);

        div(flex, justify_end, gap(2), flex_shrink_0) {
          close |= form_button(app, id("voice-switch-cancel"), "Cancel",
                               BUTTON_OUTLINE, false);
          join = form_button(app, id("voice-switch-join"), "Switch",
                             BUTTON_PRIMARY, false);
        }
      }
    }
  }

  if (civue_key_pressed(ui, CIVUE_KEY_ESCAPE)) {
    close = true;
  }

  if (join) {
    session_voice_join(s, app->voice_switch);
  }

  if (join || close) {
    app->voice_switch = 0;
  }
}

/* ========================================================================== */
/* the calls that ring. learned from the web client: each has a card over     */
/* whatever the window shows, with who calls and a button to turn the call    */
/* down and one to join it, for as long as it rings.                          */
/* ========================================================================== */

typedef struct {
  Session *s;
  const PrivateCall *call;
} Rung;

// `shown` is the private channel the window is on, whose call shows over
// its messages with the same buttons: it has no card.
static void rung_of(Session *s, Snowflake shown, Rung *rung, size_t *count) {
  const Store *store = s->store;

  if (s->state != SESSION_READY) {
    return;
  }

  for (size_t i = 0; i < store->ncalls && *count < CARDS_MOST; i++) {
    const PrivateCall *call = &store->calls[i];

    if (call_rings(store, call) && call->channel_id != shown &&
        store_channel(store, call->channel_id)) {
      rung[(*count)++] = (Rung){s, call};
    }
  }
}

// a round button of a card, its icon in `ink`, true when pressed.
static bool card_button(civue_id id, civue_icon icon, civue_color color,
                        civue_color lit, civue_color ink) {
  bool pressed = false;

  div(key(id), flex, items_center, justify_center, size(12), flex_shrink_0,
      rounded(full), style(.bg = color), hover(bg(lit)), cursor_pointer) {
    pressed = button().activated;
    glyph(icon, 22, ink);
  }

  return pressed;
}

static void call_card(App *app, const Rung *rung) {
  const Look *k = &app->look;
  Session *s = rung->s;
  Store *store = s->store;
  Snowflake channel_id = rung->call->channel_id;
  const Channel *c = store_channel(store, channel_id);
  const User *user = c->type == CHANNEL_DM && c->nrecipients
                       ? store_user(store, c->recipients[0])
                       : nullptr;
  civue_id at = id_at(id_at(id("call-card"), s->account_id), channel_id);
  char name[128], account[160] = "";
  bool decline = false, answer = false;

  store_channel_name(store, c, name, sizeof name);

  // the call of an account that is not the one shown says whose it is.
  if (s != app->session) {
    snprintf(account, sizeof account, "for %s",
             store_user_name(store_user(store, store->self_id)));
  }

  div(key(at), flex, flex_col, items_center, w(px(CARD_WIDTH)), px(4), py(6),
      rounded(xl), bg(k->surface), border(1), border_color(k->border_strong),
      shadow(lg), text_color(k->text)) {
    // a press on the card is not one on what is behind it.
    button();

    if (user) {
      avatar(app, user, 0, CARD_AVATAR, false, k->surface);
    }
    else {
      div(flex, items_center, justify_center, size(px(CARD_AVATAR)),
          flex_shrink_0, rounded(full), bg(k->primary)) {
        glyph(CIVUE_ICON(lucide, users), 36, k->on_primary);
      }
    }

    text(name, mt(4), max_w(full), truncate, text_size(px(18)),
         line_height(px(22)), font_weight(semibold), text_color(k->strong));
    text("Incoming Call", text_size(px(14)), line_height(px(18)),
         text_color(k->subtle));

    if (*account) {
      text(account, max_w(full), truncate, text_size(px(12)),
           line_height(px(16)), text_color(k->faint));
    }

    div(flex, items_center, gap(4), mt(5)) {
      decline = card_button(id_at(id("call-decline"), at),
                            CIVUE_ICON(lucide, phone_off), k->danger,
                            k->danger_hover, k->on_danger);
      answer = card_button(id_at(id("call-answer"), at),
                           CIVUE_ICON(lucide, phone), k->positive,
                           k->positive_hover, k->on_positive);
    }
  }

  if (decline) {
    session_call_decline(s, channel_id);
  }
  else if (answer) {
    alerts_open(s->account_id, channel_id, true);
  }
}

void call_cards(App *app) {
  civue_ui *ui = civue_ui_current();
  const Roster *roster = app->roster;
  Rung rung[CARDS_MOST];
  size_t count = 0;
  bool listed = false;
  // behind the settings the channel shows nothing of its call.
  const Channel *on = app->settings_open ? nullptr : app_channel(app);
  Snowflake shown = on ? on->id : 0;

  for (size_t i = 0; i < roster->naccounts; i++) {
    Session *s = roster->accounts[i].session;

    if (s) {
      listed = listed || s == app->session;
      rung_of(s, s == app->session ? shown : 0, rung, &count);
    }
  }

  // a session played from a file is no account's.
  if (!listed) {
    rung_of(app->session, shown, rung, &count);
  }

  if (!count) {
    return;
  }

  float width = (float)count * CARD_WIDTH + (float)(count - 1) * CARD_GAP;
  // under where a private channel shows its own call, whose buttons stay
  // in reach, and whole in a small window.
  float top = ui->size.y * 0.46f < 300 ? 300 : ui->size.y * 0.46f;

  if (top > ui->size.y - CARD_HEIGHT - 16) {
    top = ui->size.y - CARD_HEIGHT - 16;
  }

  overlay() {
    div(key(id("call-cards")), flex, items_start, gap(px(CARD_GAP)),
        left(px((ui->size.x - width) / 2)), top(px(top))) {
      for (size_t i = 0; i < count; i++) {
        call_card(app, &rung[i]);
      }
    }
  }
}
