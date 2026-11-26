#ifndef QUARREL_UI_AUTHORIZE_H
#define QUARREL_UI_AUTHORIZE_H

#include "app.h"
#include "discord/apps.h"

/* ========================================================================== */
/* discord's dialog that lets an application in: where it goes when it may   */
/* go to a server or to the user, what it will be allowed, the server and    */
/* the permissions of its bot, then the answer.                               */
/* ========================================================================== */

// a permission the dialog names, in the order the client lists them.
typedef struct {
  Permissions bit;
  const char *name;
} AuthorizePermission;

const AuthorizePermission *authorize_permissions(size_t *count);

typedef struct {
  Snowflake client_id;
  // what to call it until discord answered.
  const char *name;
  // what each way of installing asks for, its scopes null for a way the
  // application does not offer.
  AppInstall installs[APP_INSTALLS];
} AuthorizeRequest;

// opens the dialog over the window. everything of `request` is copied.
void authorize_open(App *app, const AuthorizeRequest *request);
// the dialog while one is open, once a frame after the page.
void authorize_dialog(App *app);
void authorize_free(App *app);

#endif
