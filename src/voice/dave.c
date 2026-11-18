#include "voice/dave.h"

#include <dave/dave.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

constexpr size_t TRANSITIONS_MAX = 8;
// the epoch a new group starts at.
constexpr uint64_t NEW_GROUP_EPOCH = 1;

typedef struct {
  Snowflake id;
  DAVEDecryptorHandle decryptor;
} User;

typedef struct {
  uint16_t id, version;
  bool pending;
} Transition;

struct Dave {
  DaveEvents e;
  Snowflake self_id, group_id;
  char self[21];
  DAVESessionHandle session;
  DAVEEncryptorHandle encryptor;

  // the users, read by the media thread while the main one changes them.
  pthread_mutex_t lock;
  User *users;
  size_t nusers;

  // transitions prepared and not executed yet, the version each goes to.
  Transition transitions[TRANSITIONS_MAX];
  uint16_t prepared_version;
  atomic_bool active;
};

uint16_t dave_max_version(void) {
  return daveMaxSupportedProtocolVersion();
}

static void on_failure(const char *source, const char *reason, void *) {
  fprintf(stderr, "quarrel: dave: %s: %s\n", source, reason);
}

// libdave prints what it does, key packages included. only what goes wrong
// is kept.
static void on_log(DAVELoggingSeverity severity, const char *file, int line,
                   const char *message) {
  if (severity == DAVE_LOGGING_SEVERITY_WARNING ||
      severity == DAVE_LOGGING_SEVERITY_ERROR) {
    fprintf(stderr, "dave: %s (%s:%d)\n", message, file, line);
  }
}

Dave *dave_new(Snowflake self_id, Snowflake group_id, const DaveEvents *e) {
  Dave *d = calloc(1, sizeof *d);

  if (d == nullptr) {
    return nullptr;
  }

  daveSetLogSinkCallback(on_log);

  d->e = *e;
  d->self_id = self_id;
  d->group_id = group_id;
  snowflake_format(self_id, d->self);
  pthread_mutex_init(&d->lock, nullptr);
  d->session = daveSessionCreate(nullptr, "", on_failure, d);
  d->encryptor = daveEncryptorCreate();
  // nothing is sent in the clear before the call says it has no dave.
  daveEncryptorSetPassthroughMode(d->encryptor, false);
  return d;
}

void dave_free(Dave *d) {
  if (d == nullptr) {
    return;
  }

  for (size_t i = 0; i < d->nusers; i++) {
    daveDecryptorDestroy(d->users[i].decryptor);
  }

  daveEncryptorDestroy(d->encryptor);
  daveSessionDestroy(d->session);
  pthread_mutex_destroy(&d->lock);
  free(d->users);
  free(d);
}

// gives a user's decryptor, or the encryptor for the account itself, the
// keys of `version`. 0 lets frames through as they are.
static void set_ratchet(Dave *d, Snowflake user_id,
                        DAVEDecryptorHandle decryptor, uint16_t version) {
  bool self = user_id == d->self_id;
  DAVEKeyRatchetHandle ratchet = nullptr;

  if (version != 0) {
    char id[21];

    ratchet = daveSessionGetKeyRatchet(d->session, snowflake_format(user_id, id));
  }

  if (self) {
    if (ratchet != nullptr) {
      daveEncryptorSetKeyRatchet(d->encryptor, ratchet);
    }

    daveEncryptorSetPassthroughMode(d->encryptor, version == 0);
    atomic_store(&d->active, version != 0);
  }
  else if (decryptor != nullptr) {
    if (ratchet != nullptr) {
      daveDecryptorTransitionToKeyRatchet(decryptor, ratchet);
    }

    daveDecryptorTransitionToPassthroughMode(decryptor, version == 0);
  }

  if (ratchet != nullptr) {
    daveKeyRatchetDestroy(ratchet);
  }
}

static User *find(Dave *d, Snowflake user_id) {
  for (size_t i = 0; i < d->nusers; i++) {
    if (d->users[i].id == user_id) {
      return &d->users[i];
    }
  }

  return nullptr;
}

void dave_add_user(Dave *d, Snowflake user_id) {
  if (user_id == d->self_id || find(d, user_id) != nullptr) {
    return;
  }

  char id[21];
  User user = {user_id, daveDecryptorCreate()};

  daveSessionAddRecognizedUser(d->session, snowflake_format(user_id, id));
  set_ratchet(d, user_id, user.decryptor, d->prepared_version);

  pthread_mutex_lock(&d->lock);
  User *grown = realloc(d->users, (d->nusers + 1) * sizeof *grown);

  if (grown != nullptr) {
    d->users = grown;
    d->users[d->nusers++] = user;
  }

  pthread_mutex_unlock(&d->lock);

  if (grown == nullptr) {
    daveDecryptorDestroy(user.decryptor);
  }
}

void dave_remove_user(Dave *d, Snowflake user_id) {
  User *user = find(d, user_id);

  if (user == nullptr) {
    return;
  }

  char id[21];
  DAVEDecryptorHandle decryptor = user->decryptor;

  daveSessionRemoveRecognizedUser(d->session, snowflake_format(user_id, id));

  pthread_mutex_lock(&d->lock);
  *user = d->users[--d->nusers];
  pthread_mutex_unlock(&d->lock);
  daveDecryptorDestroy(decryptor);
}

void dave_set_ssrc(Dave *d, uint32_t ssrc) {
  daveEncryptorAssignSsrcToCodec(d->encryptor, ssrc, DAVE_CODEC_OPUS);
}

static void send_key_package(Dave *d) {
  uint8_t *bytes = nullptr;
  size_t len = 0;

  daveSessionGetMarshalledKeyPackage(d->session, &bytes, &len);

  if (bytes != nullptr && len != 0) {
    d->e.key_package(d->e.ctx, bytes, len);
  }

  daveFree(bytes);
}

static void execute(Dave *d, uint16_t transition_id) {
  for (size_t i = 0; i < TRANSITIONS_MAX; i++) {
    Transition *t = &d->transitions[i];

    if (!t->pending || t->id != transition_id) {
      continue;
    }

    t->pending = false;

    if (t->version == 0) {
      daveSessionReset(d->session);
    }

    set_ratchet(d, d->self_id, nullptr, t->version);
    return;
  }
}

// the others' keys change at once, old and new both being read for a
// while. the account's own change when the transition is executed, at once
// for the first one.
static void prepare(Dave *d, uint16_t transition_id, uint16_t version) {
  for (size_t i = 0; i < d->nusers; i++) {
    set_ratchet(d, d->users[i].id, d->users[i].decryptor, version);
  }

  d->prepared_version = version;

  if (transition_id == 0) {
    set_ratchet(d, d->self_id, nullptr, version);
    return;
  }

  Transition *slot = &d->transitions[0];

  for (size_t i = 0; i < TRANSITIONS_MAX; i++) {
    if (!d->transitions[i].pending || d->transitions[i].id == transition_id) {
      slot = &d->transitions[i];
      break;
    }
  }

  *slot = (Transition){transition_id, version, true};
}

static void start_epoch(Dave *d, uint64_t epoch, uint16_t version) {
  if (epoch == NEW_GROUP_EPOCH) {
    daveSessionInit(d->session, version, d->group_id, d->self);
  }
}

static void init(Dave *d, uint16_t version) {
  if (version != 0) {
    start_epoch(d, NEW_GROUP_EPOCH, version);
    send_key_package(d);
  }
  else {
    prepare(d, 0, 0);
  }
}

void dave_selected(Dave *d, uint16_t version) {
  init(d, version);
}

void dave_prepare_transition(Dave *d, uint16_t transition_id,
                             uint16_t version) {
  prepare(d, transition_id, version);

  if (transition_id != 0) {
    d->e.ready_for_transition(d->e.ctx, transition_id);
  }
}

void dave_execute_transition(Dave *d, uint16_t transition_id) {
  execute(d, transition_id);
}

void dave_prepare_epoch(Dave *d, uint64_t epoch, uint16_t version) {
  start_epoch(d, epoch, version);

  if (epoch == NEW_GROUP_EPOCH) {
    send_key_package(d);
  }
}

void dave_external_sender(Dave *d, const uint8_t *bytes, size_t len) {
  daveSessionSetExternalSender(d->session, bytes, len);
}

void dave_proposals(Dave *d, const uint8_t *bytes, size_t len) {
  uint8_t *out = nullptr;
  size_t out_len = 0;

  daveSessionProcessProposals(d->session, bytes, len, &out, &out_len);

  if (out != nullptr && out_len != 0) {
    d->e.commit_welcome(d->e.ctx, out, out_len);
  }

  daveFree(out);
}

static void joined(Dave *d, uint16_t transition_id) {
  prepare(d, transition_id, daveSessionGetProtocolVersion(d->session));

  if (transition_id != 0) {
    d->e.ready_for_transition(d->e.ctx, transition_id);
  }
}

void dave_commit(Dave *d, uint16_t transition_id, const uint8_t *bytes,
                 size_t len) {
  DAVECommitResultHandle result =
    daveSessionProcessCommit(d->session, bytes, len);

  if (result == nullptr) {
    return;
  }

  bool ignored = daveCommitResultIsIgnored(result);
  bool failed = daveCommitResultIsFailed(result);

  daveCommitResultDestroy(result);

  if (ignored) {
    return;
  }

  if (failed) {
    // the group went on without this client: it asks to be let in again.
    d->e.invalid_commit_welcome(d->e.ctx, transition_id);
    init(d, daveSessionGetProtocolVersion(d->session));
    return;
  }

  joined(d, transition_id);
}

void dave_welcome(Dave *d, uint16_t transition_id, const uint8_t *bytes,
                  size_t len) {
  DAVEWelcomeResultHandle result =
    daveSessionProcessWelcome(d->session, bytes, len);

  if (result == nullptr) {
    d->e.invalid_commit_welcome(d->e.ctx, transition_id);
    send_key_package(d);
    return;
  }

  daveWelcomeResultDestroy(result);
  joined(d, transition_id);
}

size_t dave_sealed_size(Dave *d, size_t len) {
  return daveEncryptorGetMaxCiphertextByteSize(d->encryptor,
                                               DAVE_MEDIA_TYPE_AUDIO, len);
}

size_t dave_seal(Dave *d, uint32_t ssrc, const uint8_t *frame, size_t len,
                 uint8_t *out, size_t cap) {
  size_t written = 0;
  DAVEEncryptorResultCode code =
    daveEncryptorEncrypt(d->encryptor, DAVE_MEDIA_TYPE_AUDIO, ssrc, frame, len,
                         out, cap, &written);

  return code == DAVE_ENCRYPTOR_RESULT_CODE_SUCCESS ? written : 0;
}

size_t dave_open(Dave *d, Snowflake user_id, const uint8_t *frame, size_t len,
                 uint8_t *out, size_t cap) {
  size_t written = 0;
  DAVEDecryptorResultCode code = DAVE_DECRYPTOR_RESULT_CODE_MISSING_CRYPTOR;

  pthread_mutex_lock(&d->lock);
  User *user = find(d, user_id);

  if (user != nullptr &&
      daveDecryptorGetMaxPlaintextByteSize(user->decryptor,
                                           DAVE_MEDIA_TYPE_AUDIO, len) <= cap) {
    code = daveDecryptorDecrypt(user->decryptor, DAVE_MEDIA_TYPE_AUDIO, frame,
                                len, out, cap, &written);
  }

  pthread_mutex_unlock(&d->lock);
  return code == DAVE_DECRYPTOR_RESULT_CODE_SUCCESS ? written : 0;
}

bool dave_active(Dave *d) {
  return atomic_load(&d->active);
}
