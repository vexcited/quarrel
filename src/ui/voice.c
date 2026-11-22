#include "components.h"

#include <stdio.h>

#include <civue/dsl.h>

#include "voice/devices.h"
#include "voice/voice.h"
#include "widgets.h"

constexpr float MEMBER_AVATAR = 20;

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
    *color = k->danger;
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
      *color = k->danger;
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
  char where[192], detail[64] = "";

  snprintf(where, sizeof where, "%s%s%s", c && c->name ? c->name : "",
           guild && guild->name ? " / " : "",
           guild && guild->name ? guild->name : "");

  if (live && voice_latency(s->voice) >= 0) {
    snprintf(detail, sizeof detail, "%.0f ms",
             voice_latency(s->voice) * 1000);
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
                       muted ? "Unmute" : "Mute", muted, k->danger)) {
        session_voice_mute(s, !muted);
      }

      if (panel_button(app, id("voice-deafen"), sound,
                       s->voice_deaf ? "Undeafen" : "Deafen", s->voice_deaf,
                       k->danger)) {
        session_voice_deafen(s, !s->voice_deaf);
      }

      if (panel_button(app, id("voice-leave"), CIVUE_ICON(lucide, phone_off),
                       "Disconnect", true, k->danger)) {
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

static bool dialog_button(const Look *k, civue_id id, const char *label,
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

void voice_switch_dialog(App *app) {
  const Look *k = &app->look;
  civue_ui *ui = civue_ui_current();
  Session *s = app->session;
  const Channel *to = store_channel(s->store, app->voice_switch);
  const Channel *from = store_channel(s->store, s->voice_channel_id);
  bool close = false, join = false;
  char line[256];

  if (app->voice_switch == 0) {
    return;
  }

  // the call ended, or the channel went, while it asked.
  if (to == nullptr || from == nullptr) {
    app->voice_switch = 0;
    return;
  }

  snprintf(line, sizeof line,
           "You are in a call in %s. Joining %s leaves it.",
           from->name != nullptr ? from->name : "another channel",
           to->name != nullptr ? to->name : "this one");

  overlay() {
    div(key(id("voice-switch-backdrop")), left(0), top(0),
        w(px(ui->size.x)), h(px(ui->size.y)), flex, items_center,
        justify_center, bg(rgba(0x000000b3))) {
      close = button().activated;

      div(key(id("voice-switch")), flex, flex_col, gap(2), w(px(400)), p(6),
          rounded(xl), bg(k->surface), border(1), border_color(k->border),
          shadow(lg), text_color(k->text)) {
        // a press on the dialog is not one on what is behind it.
        button();

        text("Switch voice channel?", text_size(px(20)), line_height(px(24)),
             font_weight(semibold), text_color(k->strong));
        text(line, text_color(k->subtle));

        div(flex, gap(2), flex_shrink_0, mt(4)) {
          close |= dialog_button(k, id("voice-switch-cancel"), "Cancel", false);
          join = dialog_button(k, id("voice-switch-join"), "Switch", true);
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
