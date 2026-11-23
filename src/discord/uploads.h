#ifndef QUARREL_DISCORD_UPLOADS_H
#define QUARREL_DISCORD_UPLOADS_H

#include "discord/session.h"
#include "http.h"

/* ========================================================================== */
/* files sent with messages, the way the web client sends them: each file is  */
/* put in discord's bucket as soon as it is added to the composer, and the    */
/* message only names what was put there. a message waits as pending until    */
/* its files are all up.                                                      */
/* ========================================================================== */

constexpr size_t UPLOADS_MAX = 10;
// what the files of one message may weigh together.
constexpr uint64_t UPLOADS_MAX_TOTAL = 500ull << 20;

typedef enum : uint8_t {
  // asking discord where to put it.
  UPLOAD_RESERVING,
  UPLOAD_SENDING,
  UPLOAD_DONE,
  // `error` says why. it is tried again with its message.
  UPLOAD_FAILED,
  // waits for the session to be online before asking.
  UPLOAD_WAITING,
} UploadState;

typedef struct Upload {
  // never 0.
  uint32_t id;
  Snowflake channel_id;
  // the pending message it goes with, 0 while it is in the composer.
  Snowflake nonce;
  // the file read, removed once done with when `temporary`.
  char *path;
  bool temporary;
  // what the message names it, the alt text, null for none, and the media
  // type found from its name, null when unknown.
  char *filename, *description, *content_type;
  bool spoiler;
  uint64_t size;
  UploadState state;
  char *error;

  // where discord said to put it, and when.
  char *upload_url, *uploaded_filename;
  double reserved_at;
  HttpOp op;
  void *op_ctx;
  uint8_t attempts;
  // its message asked for its author to be pinged.
  bool reply_ping;
} Upload;

// adds the file at `path` to the composer of the channel and starts putting
// it in discord's bucket. `filename` is the name shown, the file's own when
// null. 0 when the file can't be read.
uint32_t uploads_add(Session *s, Snowflake channel_id, const char *path,
                     const char *filename, bool temporary);
// writes `len` bytes to a file of the caches and adds it as `filename`.
uint32_t uploads_add_bytes(Session *s, Snowflake channel_id, const void *bytes,
                           size_t len, const char *filename);
// takes a file out of the composer, stopping it or dropping it from the
// bucket.
void uploads_remove(Session *s, uint32_t id);
[[gnu::pure]] Upload *uploads_get(Session *s, uint32_t id);
// the files of a pending message, or those of the composer for 0, in the
// order they were added. answers how many there are, `out` taking at most
// `cap` of them.
size_t uploads_of(Session *s, Snowflake channel_id, Snowflake nonce,
                  Upload **out, size_t cap);
// what is up of a file.
uint64_t uploads_sent(const Upload *u);
// the largest file the user may send in the channel.
uint64_t uploads_max_size(Session *s, const Channel *c);

// for the session: the composer's files go with the pending message.
// false when it has none.
bool uploads_attach(Session *s, Snowflake channel_id, Snowflake nonce,
                    bool reply_ping);
// sends the pending message once its files are up, starting again those
// that failed.
void uploads_send(Session *s, Snowflake channel_id, Snowflake nonce);
// whether a pending message has files.
[[gnu::pure]] bool uploads_owns(Session *s, Snowflake nonce);
// stops the files of a pending message and drops it.
void uploads_cancel(Session *s, Snowflake channel_id, Snowflake nonce);
// forgets every file, for a session that ends.
void uploads_clear(Session *s);

#endif
