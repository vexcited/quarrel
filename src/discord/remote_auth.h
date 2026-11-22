#ifndef QUARREL_DISCORD_REMOTE_AUTH_H
#define QUARREL_DISCORD_REMOTE_AUTH_H

#include "discord/util.h"

/* ========================================================================== */
/* logging in by scanning a qr code with a phone already logged in, as the    */
/* web client's useAuthWebsocket does it. one RemoteAuth is one attempt: it   */
/* connects to the remote auth gateway, proves it holds an rsa key, then      */
/* hears who scanned it and the ticket that buys their token. anything that   */
/* goes wrong ends it with `failed`, and a new one is made to try again.      */
/* every callback runs on the main thread and may free the RemoteAuth.        */
/* ========================================================================== */

typedef struct RemoteAuth RemoteAuth;

// who scanned the code, as the phone sent it.
typedef struct {
  Snowflake id;
  const char *discriminator, *username;
  // null for the default avatar.
  const char *avatar;
} RemoteAuthUser;

typedef struct {
  void *ctx;
  // the handshake is done, `fingerprint` goes in the qr code.
  void (*ready)(void *ctx, const char *fingerprint);
  // the code was scanned, the phone asks its user to confirm.
  void (*scanned)(void *ctx, const RemoteAuthUser *user);
  // the phone confirmed. the socket is closed by then, the RemoteAuth is
  // kept to decrypt the token the ticket is exchanged for.
  void (*ticket)(void *ctx, const char *ticket);
  // the phone said no.
  void (*cancelled)(void *ctx);
  void (*failed)(void *ctx);
} RemoteAuthEvents;

RemoteAuth *remote_auth_new(const RemoteAuthEvents *events);
// null is fine. no callback runs after this.
void remote_auth_free(RemoteAuth *ra);
// the token in the `encrypted_token` the ticket was exchanged for, from
// malloc, null when it does not decrypt.
char *remote_auth_decrypt(RemoteAuth *ra, const char *encrypted);

#endif
