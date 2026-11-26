#include "cues.h"

#include <stdcountof.h>
#include <string.h>

#include "voice/voice.h"

// learned from the web client's SoundManager. it keeps what it last saw of
// a few stores and compares at every change: one sound for the headphones
// or else the microphone, one for the account's own connection, one for
// the others of its channel, each at four tenths of the volume.
constexpr float CUE_VOLUME = .4f;
// a channel of more than this many others tells of nobody coming or going.
constexpr uint32_t OTHERS_MOST = 25;
constexpr size_t STREAMERS_MOST = 16;
// a gateway that just connected fills the channels again, which is nobody
// coming: the others are counted afresh this long after.
constexpr double SETTLE_SECONDS = 3;

const Sound CUE_SOUNDS[] = {
  SOUND_DEAFEN,     SOUND_UNDEAFEN,   SOUND_MUTE,
  SOUND_UNMUTE,     SOUND_DISCONNECT, SOUND_USER_JOIN,
  SOUND_USER_LEAVE, SOUND_USER_MOVED, SOUND_STREAM_STARTED,
};
const size_t CUE_SOUNDS_COUNT = countof(CUE_SOUNDS);

// what was last seen of an account.
typedef struct {
  Snowflake account_id;
  Snowflake channel_id;
  bool mute, deaf;
  // its own connection is up, was ever up for this channel, and is being
  // made for the first time or up: what the client calls started.
  bool connected, ever, started;
  uint32_t moves;
  // who else is in the channel, and who of them shares a screen.
  uint32_t others;
  Snowflake streamers[STREAMERS_MOST];
  size_t nstreamers;
  // the others are not compared before this, unix seconds.
  double settled_at;
  bool ready;
} Seen;

static App *owner;
static Seen seen[16];
static size_t nseen;

static void see(const Session *s, const Seen *before, Seen *now) {
  const Store *store = s->store;
  bool failed = s->voice_error != nullptr ||
                (s->voice != nullptr && voice_status(s->voice) == VOICE_FAILED);
  bool same = before != nullptr && before->channel_id == s->voice_channel_id;

  *now = (Seen){
    .account_id = s->account_id,
    .channel_id = s->voice_channel_id,
    .mute = s->voice_mute,
    .deaf = s->voice_deaf,
    .moves = s->voice_moves,
    .ready = s->state == SESSION_READY,
    .settled_at = before != nullptr ? before->settled_at : 0,
  };

  now->connected = now->channel_id != 0 && !failed && s->voice_live &&
                   s->voice != nullptr &&
                   voice_status(s->voice) == VOICE_CONNECTED;
  now->ever = now->connected || (same && before->ever);
  // a connection that dropped is not one starting, until it is back.
  now->started = now->channel_id != 0 && !failed &&
                 (now->connected || !(same && before->ever));

  for (size_t i = 0; now->channel_id != 0 && i < store->nvoice_states; i++) {
    const VoiceState *v = &store->voice_states[i];

    if (v->channel_id != now->channel_id || v->user_id == store->self_id) {
      continue;
    }

    now->others++;

    if (v->streaming && now->nstreamers < STREAMERS_MOST) {
      now->streamers[now->nstreamers++] = v->user_id;
    }
  }
}

static bool streamed(const Seen *seen, Snowflake user_id) {
  for (size_t i = 0; i < seen->nstreamers; i++) {
    if (seen->streamers[i] == user_id) {
      return true;
    }
  }

  return false;
}

// a stage has sounds of its own, and where the idle are sent nobody is
// told of anyone.
static bool quiet_channel(const Store *store, Snowflake channel_id) {
  const Channel *c = store_channel(store, channel_id);
  const Guild *g = c != nullptr ? store_guild(store, c->guild_id) : nullptr;

  return c != nullptr && (c->type == CHANNEL_GUILD_STAGE ||
                          (g != nullptr && g->afk_channel_id == c->id));
}

static bool is_stage(const Store *store, Snowflake channel_id) {
  const Channel *c = store_channel(store, channel_id);

  return c != nullptr && c->type == CHANNEL_GUILD_STAGE;
}

static void compare(const Session *s, const Seen *before, Seen *now) {
  const Store *store = s->store;
  bool moved_channel = before->channel_id != now->channel_id;

  // the headphones say it for the microphone they take along.
  if (before->deaf != now->deaf) {
    sounds_play(now->deaf ? SOUND_DEAFEN : SOUND_UNDEAFEN, CUE_VOLUME);
  }
  else if (before->mute != now->mute) {
    sounds_play(now->mute ? SOUND_MUTE : SOUND_UNMUTE, CUE_VOLUME);
  }

  if (!before->ready && now->ready) {
    now->settled_at = store_now(store) + SETTLE_SECONDS;
  }

  // someone moved the account: that alone is told, and the connection
  // made again for it is no call starting.
  if (before->moves != now->moves) {
    sounds_play(SOUND_USER_MOVED, CUE_VOLUME);
    now->started = now->channel_id != 0;
    return;
  }

  // the account's own connection. the client tells of it as it starts
  // rather than once it is up, and again each time it comes back.
  bool began = now->started && (moved_channel || !before->started);

  if (began) {
    if (!is_stage(store, now->channel_id)) {
      sounds_play(SOUND_USER_JOIN, CUE_VOLUME);
    }

    return;
  }

  if (before->connected && !now->connected) {
    if (!is_stage(store, before->channel_id)) {
      sounds_play(SOUND_DISCONNECT, CUE_VOLUME);
    }

    return;
  }

  // the others of a channel the account stayed in.
  if (moved_channel || now->channel_id == 0 || !now->ready ||
      store_now(store) < now->settled_at ||
      quiet_channel(store, now->channel_id)) {
    return;
  }

  for (size_t i = 0; i < now->nstreamers; i++) {
    if (!streamed(before, now->streamers[i])) {
      sounds_play(SOUND_STREAM_STARTED, CUE_VOLUME);
      return;
    }
  }

  if (before->others <= OTHERS_MOST && now->others != before->others) {
    sounds_play(now->others > before->others ? SOUND_USER_JOIN
                                             : SOUND_USER_LEAVE,
                CUE_VOLUME);
  }
}

static void follow(const Session *s) {
  if (s == nullptr || s->account_id == 0) {
    return;
  }

  Seen *before = nullptr;

  for (size_t i = 0; i < nseen; i++) {
    if (seen[i].account_id == s->account_id) {
      before = &seen[i];
    }
  }

  Seen now;

  see(s, before, &now);

  // an account seen for the first time has nothing to compare with.
  if (before == nullptr) {
    if (nseen < countof(seen)) {
      seen[nseen++] = now;
    }

    return;
  }

  compare(s, before, &now);
  *before = now;
}

void cues_follow(void) {
  if (owner == nullptr) {
    return;
  }

  const Roster *roster = owner->roster;

  for (size_t i = 0; i < roster->naccounts; i++) {
    follow(roster->accounts[i].session);
  }

  // an account that left takes what was seen of it along.
  for (size_t i = nseen; i > 0; i--) {
    bool kept = false;

    for (size_t j = 0; j < roster->naccounts; j++) {
      kept = kept || (roster->accounts[j].id == seen[i - 1].account_id &&
                      roster->accounts[j].session != nullptr);
    }

    if (!kept) {
      seen[i - 1] = seen[--nseen];
    }
  }
}

void cues_start(App *app) {
  owner = app;

  // a call does not wait for its first sounds to be fetched.
  for (size_t i = 0; i < CUE_SOUNDS_COUNT; i++) {
    if (sounds_enabled(CUE_SOUNDS[i])) {
      sounds_fetch(CUE_SOUNDS[i]);
    }
  }
}
