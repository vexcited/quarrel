#include "server.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <civue/dsl.h>

#include "widgets.h"

// what a role without a color shows, and discord's swatches of role colors,
// the light ones over the dark ones.
constexpr uint32_t ROLE_DEFAULT_COLOR = 0x99aab5;
static const uint32_t SWATCHES[20] = {
  0x1abc9c, 0x2ecc71, 0x3498db, 0x9b59b6, 0xe91e63, 0xf1c40f, 0xe67e22,
  0xe74c3c, 0x95a5a6, 0x607d8b, 0x11806a, 0x1f8b4c, 0x206694, 0x71368a,
  0xad1457, 0xc27c0e, 0xa84300, 0x992d22, 0x979c9f, 0x546e7a,
};

constexpr float ROLE_ROW = 61;
constexpr float COLUMN_ROW = 34;
constexpr float MEMBER_ROW = 40;
// a press becomes a drag once the pointer went this far.
constexpr float DRAG_SLOP = 4;
constexpr size_t ADD_MOST = 30;

/* ========================================================================== */
/* the permissions, grouped and worded as discord's role editor has them.     */
/* ========================================================================== */

typedef struct {
  Permissions bit;
  const char *title, *about;
  // only shown in a community.
  bool community;
} PermissionRow;

typedef struct {
  const char *title;
  const PermissionRow *rows;
  size_t count;
  bool community;
} PermissionGroup;

static const PermissionRow GENERAL[] = {
  {PERM_VIEW_CHANNEL, "View Channels",
   "Allows members to view channels by default (excluding private "
   "channels).",
   false},
  {PERM_MANAGE_CHANNELS, "Manage Channels",
   "Allows members to create, edit, or delete channels.", false},
  {PERM_MANAGE_ROLES, "Manage Roles",
   "Allows members to create new roles and edit or delete roles lower than "
   "their highest role. Also allows members to change permissions of "
   "individual channels that they have access to.",
   false},
  {PERM_CREATE_EXPRESSIONS, "Create Expressions",
   "Allows members to add custom emoji, stickers, and sounds in this "
   "server.",
   false},
  {PERM_MANAGE_EXPRESSIONS, "Manage Expressions",
   "Allows members to edit or remove custom emoji, stickers, and sounds in "
   "this server.",
   false},
  {PERM_VIEW_AUDIT_LOG, "View Audit Log",
   "Allows members to view a record of who made which changes in this "
   "server.",
   false},
  {PERM_VIEW_GUILD_INSIGHTS, "View Server Insights",
   "Allows members to view Server Insights, which shows data on community "
   "growth, engagement, and more. This will allow them to see certain data "
   "about channel activity, even for channels they cannot access.",
   true},
  {PERM_MANAGE_WEBHOOKS, "Manage Webhooks",
   "Allows members to create, edit, or delete webhooks, which can post "
   "messages from other apps or sites into this server.",
   false},
  {PERM_MANAGE_GUILD, "Manage Server",
   "Allow members to change this server's name, switch regions, view all "
   "invites, add apps to this server and create and update AutoMod rules.",
   false},
};

static const PermissionRow MEMBERSHIP[] = {
  {PERM_CREATE_INSTANT_INVITE, "Create Invite",
   "Allows members to invite new people to this server.", false},
  {PERM_CHANGE_NICKNAME, "Change Nickname",
   "Allows members to change their own nickname, a custom name for just "
   "this server.",
   false},
  {PERM_MANAGE_NICKNAMES, "Manage Nicknames",
   "Allows members to change the nicknames of other members.", false},
  {PERM_KICK_MEMBERS, "Kick, Approve, and Reject Members",
   "Kick will remove other members from this server. Kicked members will be "
   "able to rejoin if they have another invite. If the server enables "
   "Member Requirements, this permission enables the ability to approve or "
   "reject members who request to join.",
   false},
  {PERM_BAN_MEMBERS, "Ban Members",
   "Allows members to permanently ban and delete the message history of "
   "other members from this server.",
   false},
  {PERM_MODERATE_MEMBERS, "Timeout Members",
   "When you put a user in timeout they will not be able to send messages in "
   "chat, reply within threads, react to messages, or speak in voice or "
   "Stage channels.",
   false},
};

static const PermissionRow TEXT[] = {
  {PERM_SEND_MESSAGES, "Send Messages and Create Posts",
   "Allow members to send messages in text channels and create posts in "
   "forum channels.",
   false},
  {PERM_SEND_MESSAGES_IN_THREADS, "Send Messages in Threads and Posts",
   "Allow members to send messages in threads and in posts on forum "
   "channels.",
   false},
  {PERM_CREATE_PUBLIC_THREADS, "Create Public Threads",
   "Allow members to create threads that everyone in a channel can view.",
   false},
  {PERM_CREATE_PRIVATE_THREADS, "Create Private Threads",
   "Allow members to create invite-only threads.", false},
  {PERM_EMBED_LINKS, "Embed Links",
   "Allows links that members share to show embedded content in text "
   "channels.",
   false},
  {PERM_ATTACH_FILES, "Attach Files",
   "Allows members to upload files or media in text channels.", false},
  {PERM_ADD_REACTIONS, "Add Reactions",
   "Allows members to add new emoji reactions to a message. If this "
   "permission is disabled, members can still react using any existing "
   "reactions on a message.",
   false},
  {PERM_USE_EXTERNAL_EMOJIS, "Use External Emoji",
   "Allows members to use emoji from other servers, if they're a Discord "
   "Nitro member.",
   false},
  {PERM_USE_EXTERNAL_STICKERS, "Use External Stickers",
   "Allows members to use stickers from other servers, if they're a Discord "
   "Nitro member.",
   false},
  {PERM_MENTION_EVERYONE, "Mention @everyone, @here, and All Roles",
   "Allows members to use @everyone (everyone in the server) or @here (only "
   "online members in that channel). They can also @mention all roles, even "
   "if the role's \"Allow anyone to mention this role\" permission is "
   "disabled.",
   false},
  {PERM_MANAGE_MESSAGES, "Manage Messages",
   "Allows members to delete or remove embeds from messages by other "
   "members.",
   false},
  {PERM_PIN_MESSAGES, "Pin Messages",
   "Allows members to pin or unpin any message.", false},
  {PERM_BYPASS_SLOWMODE, "Bypass Slowmode",
   "Allows members to send messages without being affected by slowmode.",
   false},
  {PERM_MANAGE_THREADS, "Manage Threads and Posts",
   "Allows members to rename, delete, close, and turn on slow mode for "
   "threads and posts. They can also view private threads.",
   false},
  {PERM_READ_MESSAGE_HISTORY, "Read Message History",
   "Allows members to read previous messages sent in channels. If this "
   "permission is disabled, members only see messages sent when they are "
   "online. This does not fully apply to threads and forum posts.",
   false},
  {PERM_SEND_TTS_MESSAGES, "Send Text-to-Speech Messages",
   "Allows members to send text-to-speech messages by starting a message "
   "with /tts. These messages can be heard by anyone focused on the "
   "channel.",
   false},
  {PERM_SEND_VOICE_MESSAGES, "Send Voice Messages",
   "Allows members to send voice messages.", false},
  {PERM_SEND_POLLS, "Create Polls", "Allows members to create polls.", false},
};

static const PermissionRow VOICE[] = {
  {PERM_CONNECT, "Connect",
   "Allows members to join voice channels and hear others.", false},
  {PERM_SPEAK, "Speak",
   "Allows members to talk in voice channels. If this permission is "
   "disabled, members are default muted until somebody with the \"Mute "
   "Members\" permission un-mutes them.",
   false},
  {PERM_STREAM, "Video",
   "Allows members to share their video, screen share, or stream a game in "
   "this server.",
   false},
  {PERM_USE_SOUNDBOARD, "Use Soundboard",
   "Allows members to send sounds from server soundboard.", false},
  {PERM_USE_EXTERNAL_SOUNDS, "Use External Sounds",
   "Allows members to use sounds from other servers, if they're a Discord "
   "Nitro member.",
   false},
  {PERM_USE_VAD, "Use Voice Activity",
   "Allows members to speak in voice channels by simply talking. If this "
   "permission is disabled, members are required to use Push-to-talk. Good "
   "for controlling background noise or noisy members.",
   false},
  {PERM_PRIORITY_SPEAKER, "Priority Speaker",
   "Allows members to be more easily heard in voice channels. When "
   "activated, the volume of others without this permission will be "
   "automatically lowered.",
   false},
  {PERM_MUTE_MEMBERS, "Mute Members",
   "Allows members to mute other members in voice channels for everyone.",
   false},
  {PERM_DEAFEN_MEMBERS, "Deafen Members",
   "Allows members to deafen other members in voice channels, which means "
   "they won't be able to speak or hear others.",
   false},
  {PERM_MOVE_MEMBERS, "Move Members",
   "Allows members to disconnect or move other members between voice "
   "channels that the member with this permission has access to.",
   false},
  {PERM_SET_VOICE_CHANNEL_STATUS, "Set Voice Channel Status",
   "Allows members to create and edit voice channel status.", false},
};

static const PermissionRow APPS[] = {
  {PERM_USE_APPLICATION_COMMANDS, "Use Application Commands",
   "Allows members to use commands from applications, including slash "
   "commands and context menu commands.",
   false},
  {PERM_USE_EMBEDDED_ACTIVITIES, "Use Activities",
   "Allows members to use Activities.", false},
  {PERM_USE_EXTERNAL_APPS, "Use External Apps",
   "Allows apps that members have added to their account to post messages. "
   "When disabled, the messages will be private.",
   false},
};

static const PermissionRow STAGE[] = {
  {PERM_REQUEST_TO_SPEAK, "Request to Speak",
   "Allow requests to speak in Stage channels. Stage moderators manually "
   "approve or deny each request.",
   true},
};

static const PermissionRow EVENTS[] = {
  {PERM_CREATE_EVENTS, "Create Events", "Allows members to create events.",
   false},
  {PERM_MANAGE_EVENTS, "Manage Events",
   "Allows members to edit and cancel events.", false},
};

static const PermissionRow ADVANCED[] = {
  {PERM_ADMINISTRATOR, "Administrator",
   "Members with this permission will have every permission and will also "
   "bypass all channel specific permissions or restrictions (for example, "
   "these members would get access to all private channels). This is a "
   "dangerous permission to grant.",
   false},
};

#define GROUP(title, rows, community) \
  {title, rows, sizeof rows / sizeof *rows, community}

static const PermissionGroup GROUPS[] = {
  GROUP("General Server Permissions", GENERAL, false),
  GROUP("Membership Permissions", MEMBERSHIP, false),
  GROUP("Text Channel Permissions", TEXT, false),
  GROUP("Voice Channel Permissions", VOICE, false),
  GROUP("Apps Permissions", APPS, false),
  GROUP("Stage Channel Permissions", STAGE, true),
  GROUP("Events Permissions", EVENTS, false),
  GROUP("Advanced Permissions", ADVANCED, false),
};

/* ========================================================================== */
/* the drafts, which wait for the notice's save.                              */
/* ========================================================================== */

static void draft_free(RoleDraft *d) {
  free(d->name);
  *d = (RoleDraft){};
}

void roles_forget(ServerUi *ui) {
  for (size_t i = 0; i < ui->nroles; i++) {
    draft_free(&ui->roles[i]);
  }

  free(ui->roles);
  free(ui->order);
  free(ui->role_search);
  free(ui->perm_search);
  free(ui->picker_search);
  free(ui->picker_asked);
  ui->roles = nullptr;
  ui->order = nullptr;
  ui->nroles = ui->norder = 0;
  ui->role_search = ui->perm_search = nullptr;
  ui->picker_search = ui->picker_asked = nullptr;
  ui->role_id = 0;
  ui->role_tab = ROLE_TAB_DISPLAY;
  ui->role_drag = (Drag){};
  ui->role_menu = 0;
  ui->picker_open = false;
  ui->picks.len = 0;
}

static RoleDraft *draft_of(ServerUi *ui, Snowflake id) {
  for (size_t i = 0; i < ui->nroles; i++) {
    if (ui->roles[i].id == id) {
      return &ui->roles[i];
    }
  }

  return nullptr;
}

// the draft of a role, made from it the first time.
static RoleDraft *draft_for(ServerUi *ui, const Role *r) {
  RoleDraft *d = draft_of(ui, r->id);

  if (d != nullptr) {
    return d;
  }

  ui->roles = realloc(ui->roles, (ui->nroles + 1) * sizeof *ui->roles);
  d = &ui->roles[ui->nroles++];
  *d = (RoleDraft){
    .id = r->id,
    .name = str_dup(r->name),
    .color = r->color,
    .hoist = r->hoist,
    .mentionable = r->mentionable,
    .permissions = r->permissions,
  };

  return d;
}

static uint32_t draft_changes(const RoleDraft *d, const Role *r) {
  uint32_t f = 0;

  if (r == nullptr) {
    return 0;
  }

  f |= strcmp(d->name ? d->name : "", r->name ? r->name : "") != 0
         ? ROLE_EDIT_NAME
         : 0;
  f |= d->color != r->color ? ROLE_EDIT_COLOR : 0;
  f |= d->hoist != r->hoist ? ROLE_EDIT_HOIST : 0;
  f |= d->mentionable != r->mentionable ? ROLE_EDIT_MENTIONABLE : 0;
  f |= d->permissions != r->permissions ? ROLE_EDIT_PERMISSIONS : 0;
  return f;
}

// whether the order dragged differs from the guild's.
static bool order_changed(const ServerUi *ui, const Guild *g) {
  if (ui->norder == 0) {
    return false;
  }

  if (ui->norder != g->nroles) {
    return true;
  }

  for (size_t i = 0; i < g->nroles; i++) {
    if (ui->order[i] != g->roles[i]->id) {
      return true;
    }
  }

  return false;
}

bool roles_dirty(const ServerUi *ui, const Guild *g) {
  for (size_t i = 0; i < ui->nroles; i++) {
    if (draft_changes(&ui->roles[i], store_role(g, ui->roles[i].id)) != 0) {
      return true;
    }
  }

  return order_changed(ui, g);
}

// as discord's client saves: the order first, then every role edited.
void roles_save(App *app, ServerUi *ui, Guild *g) {
  free(ui->error);
  ui->error = nullptr;

  for (size_t i = 0; i < ui->nroles; i++) {
    const RoleDraft *d = &ui->roles[i];

    if ((draft_changes(d, store_role(g, d->id)) & ROLE_EDIT_NAME) &&
        (d->name == nullptr || *d->name == 0)) {
      str_set(&ui->error, "Roles need a name.");
      return;
    }
  }

  if (order_changed(ui, g)) {
    admin_order_roles(app->session, g->id, ui->order, ui->norder);
  }

  free(ui->order);
  ui->order = nullptr;
  ui->norder = 0;

  for (size_t i = 0; i < ui->nroles; i++) {
    const RoleDraft *d = &ui->roles[i];
    const Role *r = store_role(g, d->id);
    RoleEdit edit = {
      .fields = draft_changes(d, r),
      .name = d->name,
      .color = d->color,
      .hoist = d->hoist,
      .mentionable = d->mentionable,
      .permissions = d->permissions,
    };

    if (r != nullptr && edit.fields != 0) {
      admin_edit_role(app->session, g->id, r->id, &edit);
    }
  }
}

/* ========================================================================== */
/* who may do what with roles.                                                */
/* ========================================================================== */

static bool is_owner(App *app, const Guild *g) {
  return g->owner_id == app->session->store->self_id;
}

// discord's rule: a role strictly under the user's highest one, or any for
// the owner. @everyone's permissions stay editable.
static bool role_editable(App *app, const Guild *g, const Role *r) {
  Store *store = app->session->store;

  if (!(store_guild_permissions(store, g) & PERM_MANAGE_ROLES)) {
    return false;
  }

  return r->id == g->id || is_owner(app, g) ||
         r->position < store_highest_position(store, g);
}

static bool role_movable(App *app, const Guild *g, const Role *r) {
  return r->id != g->id && role_editable(app, g, r);
}

static bool role_managed(const Role *r) {
  return r->managed || r->bot_id || r->integration_id || r->premium_subscriber;
}

static civue_color color_of(uint32_t color) {
  return rgb(color ? color : ROLE_DEFAULT_COLOR);
}

// what the user may do in the guild if role `id` granted `granted`.
static Permissions mine_if(App *app, const Guild *g, Snowflake id,
                           Permissions granted) {
  Store *store = app->session->store;
  const Member *me = store_member(g, store->self_id);
  const Role *everyone = store_role(g, g->id);
  Snowflake ids[256];
  Permissions grants[256];
  size_t n = 0;

  for (size_t i = 0; me != nullptr && i < me->nroles && n < 256; i++) {
    const Role *r = store_role(g, me->roles[i]);

    if (r != nullptr) {
      ids[n] = r->id;
      grants[n++] = r->id == id ? granted : r->permissions;
    }
  }

  PermissionSubject who = {
    .guild_id = g->id,
    .owner_id = g->owner_id,
    .user_id = store->self_id,
    .everyone = id == g->id ? granted
                : everyone  ? everyone->permissions
                            : 0,
    .role_ids = ids,
    .role_permissions = grants,
    .nroles = n,
  };

  return permissions_base(&who);
}

// why a permission of a role cannot be changed, null when it can.
static const char *locked_why(App *app, const Guild *g, const RoleDraft *d,
                              Permissions bit) {
  Store *store = app->session->store;

  if (is_owner(app, g)) {
    return nullptr;
  }

  if (!(store_guild_permissions(store, g) & bit)) {
    return "You cannot modify this permission because none of your roles "
           "have it.";
  }

  bool on = (d->permissions & bit) != 0;

  if (on && !(mine_if(app, g, d->id, d->permissions & ~bit) & bit)) {
    return "You cannot modify this permission because removing it would "
           "remove it from you.";
  }

  return nullptr;
}

/* ========================================================================== */
/* the order of the list, the one dragged until it is saved.                  */
/* ========================================================================== */

// every role, highest first and @everyone last.
static size_t ordered(const ServerUi *ui, const Guild *g, Role **out,
                      size_t cap) {
  size_t n = 0;

  if (ui->norder == 0) {
    for (size_t i = 0; i < g->nroles && n < cap; i++) {
      out[n++] = g->roles[i];
    }

    return n;
  }

  // a role made since the drag goes right over @everyone, gone ones go.
  for (size_t i = 0; i < ui->norder && n < cap; i++) {
    Role *r = store_role(g, ui->order[i]);

    if (r != nullptr && r->id != g->id) {
      out[n++] = r;
    }
  }

  for (size_t i = 0; i < g->nroles && n < cap; i++) {
    bool known = false;

    for (size_t j = 0; j < ui->norder; j++) {
      known = known || ui->order[j] == g->roles[i]->id;
    }

    if (!known && g->roles[i]->id != g->id) {
      out[n++] = g->roles[i];
    }
  }

  Role *everyone = store_role(g, g->id);

  if (everyone != nullptr && n < cap) {
    out[n++] = everyone;
  }

  return n;
}

// the role at `from` moved to `to`, as the order waiting to be saved.
static void move_role(ServerUi *ui, Role **roles, size_t count, size_t from,
                      size_t to, const Guild *g) {
  Role *all[256];
  size_t n = 0;

  // the list shown may be a part, the order is of every role.
  (void)count;

  n = ordered(ui, g, all, 256);

  size_t a = n, b = n;

  for (size_t i = 0; i < n; i++) {
    a = all[i] == roles[from] ? i : a;
    b = all[i] == roles[to] ? i : b;
  }

  if (a == n || b == n) {
    return;
  }

  Role *moved = all[a];

  if (a < b) {
    memmove(&all[a], &all[a + 1], (b - a) * sizeof *all);
  }
  else {
    memmove(&all[b + 1], &all[b], (a - b) * sizeof *all);
  }

  all[b] = moved;

  free(ui->order);
  ui->order = calloc(n, sizeof *ui->order);
  ui->norder = n;

  for (size_t i = 0; i < n; i++) {
    ui->order[i] = all[i]->id;
  }
}

/* ========================================================================== */
/* dragging roles.                                                            */
/* ========================================================================== */

static void drag_follow(Drag *d, Snowflake id, const civue_button_state *state,
                        bool movable) {
  civue_ui *ui = civue_ui_current();

  if (state->response.pressed && movable) {
    d->pressed = id;
    d->from = ui->input.mouse;
  }

  if (d->pressed == id && d->id == 0 && ui->input.down[0]) {
    float dx = ui->input.mouse.x - d->from.x;
    float dy = ui->input.mouse.y - d->from.y;

    if (dx * dx + dy * dy > DRAG_SLOP * DRAG_SLOP) {
      d->id = id;
    }
  }
}

// the drag of a list of roles, `inset` the room before its line: the line
// where the role goes, and the move once it is let go.
static void roles_drop(App *app, ServerUi *ui, Guild *g, Role **roles,
                       const civue_rect *rects, size_t count, float inset) {
  const Look *k = &app->look;
  civue_ui *cui = civue_ui_current();
  Drag *d = &ui->role_drag;
  size_t from = count, to = count;

  if (!cui->input.down[0] && d->id == 0) {
    d->pressed = 0;
  }

  if (d->id == 0) {
    return;
  }

  for (size_t i = 0; i < count; i++) {
    from = roles[i]->id == d->id ? i : from;

    // discord drops on the row under the pointer, when it may move.
    if (cui->input.mouse.y >= rects[i].y0 && cui->input.mouse.y < rects[i].y1 &&
        role_movable(app, g, roles[i])) {
      to = i;
    }
  }

  if (from == count) {
    // the drag is the other list's.
    return;
  }

  if (to != count && to != from) {
    civue_rect r = rects[to];
    // above the target when the role comes from under it.
    float y = to < from ? (float)r.y0 : (float)r.y1;

    overlay() {
      civue_rect bar = civue_rect_from(r.x0 + inset, y - 2, r.x1 - 8, y + 2);

      civue_shadow(cui, bar, 2, 3, civue_v2(0, 0), rgba(0x00000066));
      fill(bar, k->drop, 2);
    }
  }

  civue_set_cursor(cui, CIVUE_CURSOR_GRABBING);
  civue_request_repaint(cui);

  if (cui->input.down[0] && !cui->input.released[0]) {
    return;
  }

  *d = (Drag){};

  if (to != count && to != from) {
    move_role(ui, roles, count, from, to, g);
  }
}

/* ========================================================================== */
/* the list of roles.                                                         */
/* ========================================================================== */

static void open_role(App *app, ServerUi *ui, Guild *g, Snowflake id,
                      RoleTab tab) {
  const Role *r = store_role(g, id);

  if (r == nullptr) {
    return;
  }

  ui->role_id = id;
  ui->role_tab = id == g->id ? ROLE_TAB_PERMISSIONS : tab;
  draft_for(ui, r);
  free(ui->perm_search);
  free(ui->member_search);
  ui->perm_search = ui->member_search = nullptr;

  if (id != g->id) {
    admin_fetch_members(app->session, g->id, id);
  }
}

static void everyone_card(App *app, ServerUi *ui, Guild *g) {
  const Look *k = &app->look;
  bool open = false;

  div(key(id("roles-everyone")), flex, items_center, flex_shrink_0, p(4), pr(6),
      rounded(lg), bg(k->surface), border(1), border_color(k->border),
      text_color(k->subtle), hover(bg(k->hover), text_color(k->strong)),
      cursor_pointer) {
    open = button().activated;

    div(flex, items_center, justify_center, size(8), flex_shrink_0,
        rounded(full), bg(k->panel)) {
      icon(lucide, users, size(5));
    }

    div(flex, flex_col, gap(1), flex_1, min_w(0), ml(4)) {
      text("Default Permissions", font_weight(semibold),
           text_color(k->strong));
      text("@everyone \xe2\x80\xa2 applies to all server members",
           text_size(px(12)), line_height(px(16)));
    }

    icon(lucide, chevron_right, size(5), flex_shrink_0);
  }

  if (open) {
    open_role(app, ui, g, g->id, ROLE_TAB_PERMISSIONS);
  }
}

static void role_shield(civue_color color) {
  civue_node_icon(civue_ui_current(), CIVUE_ICON(lucide, shield_user),
                  &CIVUE_NODE_STYLE(.w = civue_px(24), .h = civue_px(24),
                                    .text_color = color,
                                    .has_text_color = true, .shrink = 0,
                                    .has_shrink = true));
}

static void roles_list(App *app, ServerUi *ui, Guild *g) {
  const Look *k = &app->look;
  civue_ui *cui = civue_ui_current();
  Admin *a = admin_of(app->session, g->id);
  Role *all[256], *roles[256];
  civue_rect rects[256];
  size_t nall = ordered(ui, g, all, 256), n = 0;
  bool searching = ui->role_search && *ui->role_search;
  char title[64];

  if (a->ncounts == 0 && !a->counts_loading) {
    admin_fetch_counts(app->session, g->id);
  }

  for (size_t i = 0; i < nall; i++) {
    if (all[i]->id == g->id) {
      continue;
    }

    if (searching &&
        !(all[i]->name && strcasestr(all[i]->name, ui->role_search))) {
      continue;
    }

    roles[n++] = all[i];
  }

  form_heading(k, "Roles",
               "Use roles to group your server members and assign "
               "permissions.");
  div(flex_shrink_0, h(4)) {}
  everyone_card(app, ui, g);

  div(flex, items_center, gap(4), flex_shrink_0, mt(8)) {
    div(flex_1, min_w(0)) {
      form_input(app, id("roles-search"), &ui->role_search, "Search Roles",
                 nullptr, 0);
    }

    if (form_button_small(app, id("roles-create"),
                          a->role.status == ADMIN_BUSY ? "Creating..."
                                                       : "Create Role",
                          BUTTON_PRIMARY, a->role.status == ADMIN_BUSY)) {
      admin_create_role(app->session, g->id);
    }
  }

  text("Members use the color of the highest role they have on this list. "
       "Drag roles to reorder them.",
       flex_shrink_0, mt(2), text_size(px(14)), line_height(px(18)),
       text_color(k->text));

  const char *error = a->role.status == ADMIN_FAILED   ? a->role.error
                      : a->move.status == ADMIN_FAILED ? a->move.error
                                                       : nullptr;

  if (error != nullptr) {
    text(error, flex_shrink_0, mt(2), text_size(px(14)), line_height(px(18)),
         text_color(k->critical_text));
  }

  snprintf(title, sizeof title, "Roles \xc2\xb7 %zu", n);

  div(flex, items_center, flex_shrink_0, mt(8), mb(2), text_size(px(14)),
      line_height(px(18)), font_weight(semibold), text_color(k->text)) {
    div(w(px(32)), flex_shrink_0) {}
    text(title, style(.grow = 0, .shrink = 1, .has_shrink = true,
                      .basis = civue_px(273)));
    text("Members", text_right, ml(4), w(px(112)), flex_shrink_0);
    div(flex_1) {}
  }

  div(flex, flex_col, flex_shrink_0, border_t(1), border_color(k->border)) {
    for (size_t i = 0; i < n; i++) {
      Role *r = roles[i];
      bool movable = !searching && role_movable(app, g, r);
      bool locked = !role_editable(app, g, r);
      bool dragged = ui->role_drag.id == r->id;
      bool open = false, members = false;
      const RoleDraft *d = draft_of(ui, r->id);
      char count[16];

      snprintf(count, sizeof count, "%u", admin_count(a, r->id));

      div(key(id_at(id("roles-row"), r->id)), relative, flex, items_center,
          flex_shrink_0, h(px(ROLE_ROW)), pr(2), rounded(md), border_b(1),
          border_color(k->border),
          style(.bg = dragged ? k->hover : CIVUE_CLEAR),
          hover(bg(ui->role_drag.id ? CIVUE_CLEAR : k->hover)),
          cursor_pointer) {
        civue_button_state state = button();

        rects[i] = here();
        drag_follow(&ui->role_drag, r->id, &state, movable);
        open = state.activated && ui->role_drag.id == 0;

        if (state.response.right_clicked) {
          ui->role_menu = r->id;
          ui->role_menu_at =
            civue_rect_from(cui->input.mouse.x, cui->input.mouse.y,
                            cui->input.mouse.x, cui->input.mouse.y);
        }

        // the dragged row is an empty box until it lands.
        if (dragged) {
          continue;
        }

        div(flex, items_center, justify_center, w(px(32)), flex_shrink_0,
            h(full),
            text_color(movable && state.response.hovered ? k->subtle
                                                         : CIVUE_CLEAR)) {
          icon(lucide, grip_vertical, size(4));
        }

        div(flex, items_center, gap(px(10)), min_w(0),
            style(.grow = 0, .shrink = 1, .has_shrink = true,
                  .basis = civue_px(273))) {
          role_shield(color_of(d ? d->color : r->color));
          text(d && d->name ? d->name : r->name ? r->name : "", min_w(0),
               truncate, font_weight(medium), text_color(k->strong));

          if (locked) {
            icon(lucide, lock, size(4), flex_shrink_0, text_color(k->muted));
          }
        }

        div(key(id_at(id("roles-count"), r->id)), flex, items_center,
            justify_end, gap(1), ml(4), w(px(112)), flex_shrink_0,
            text_color(k->subtle), hover(text_color(k->strong))) {
          members = button().activated;
          text(count);
          icon(lucide, user, size(5));
        }

        div(flex, items_center, justify_end, gap(2), flex_1, ml(4)) {
          div(key(id_at(id("roles-edit"), r->id)), flex, items_center,
              justify_center, size(8), rounded(full), bg(k->hover),
              hover(bg(k->pressed)), text_color(k->strong)) {
            open |= button().activated;

            if (locked) {
              icon(lucide, eye, size(4));
            }
            else {
              icon(lucide, pencil, size(4));
            }
          }

          div(key(id_at(id("roles-more"), r->id)), flex, items_center,
              justify_center, size(8), rounded(full), bg(k->hover),
              hover(bg(k->pressed)), text_color(k->strong)) {
            if (button().activated) {
              ui->role_menu = r->id;
              ui->role_menu_at = here();
            }

            icon(lucide, ellipsis, size(4));
          }
        }
      }

      if (members) {
        open_role(app, ui, g, r->id, ROLE_TAB_MEMBERS);
      }
      else if (open) {
        open_role(app, ui, g, r->id, ROLE_TAB_DISPLAY);
      }
    }
  }

  if (n == 0) {
    div(flex, items_center, gap(2), mt(6), text_color(k->muted)) {
      icon(lucide, users, size(5));
      text("No roles", font_weight(semibold));
    }
  }

  roles_drop(app, ui, g, roles, rects, n, 32);

  // a role was made: it opens on its display.
  if (a->role.status == ADMIN_DONE && a->role.made != 0) {
    Snowflake made = a->role.made;

    admin_clear(a, &a->role);
    open_role(app, ui, g, made, ROLE_TAB_DISPLAY);
  }
}

/* ========================================================================== */
/* the editor.                                                                */
/* ========================================================================== */

// the column of roles at the left of the editor, which also reorders.
static void editor_column(App *app, ServerUi *ui, Guild *g, bool *back) {
  const Look *k = &app->look;
  Admin *a = admin_of(app->session, g->id);
  Role *roles[256];
  civue_rect rects[256];
  size_t n = ordered(ui, g, roles, 256);

  div(flex, flex_col, w(px(232)), flex_shrink_0, border_r(1),
      border_color(k->border)) {
    div(flex, items_center, justify_between, flex_shrink_0, ml(4), mr(2),
        mb(2), px(2), pb(4)) {
      div(key(id("roles-back")), flex, items_center, gap(2), h(8),
          font_weight(semibold), text_color(k->text),
          hover(text_color(k->strong)), cursor_pointer) {
        *back = button().activated;
        icon(lucide, chevron_left, size(5));
        text("Back");
      }

      if (icon_button(app, id("roles-new"), CIVUE_ICON(lucide, plus), 28, 20,
                      "Create Role") &&
          a->role.status != ADMIN_BUSY) {
        admin_create_role(app->session, g->id);
      }
    }

    div(flex, flex_col, pl(10), pr(4)) {
      for (size_t i = 0; i < n; i++) {
        Role *r = roles[i];
        bool on = ui->role_id == r->id;
        bool everyone = r->id == g->id;
        bool locked = !role_editable(app, g, r);
        const RoleDraft *d = draft_of(ui, r->id);
        const char *name = everyone      ? "@everyone"
                           : d && d->name ? d->name
                           : r->name      ? r->name
                                          : "";

        div(key(id_at(id("roles-column"), r->id)), flex, items_center,
            gap(1), flex_shrink_0, h(px(COLUMN_ROW)), px(1), mb(px(2)),
            ml(px(-6)), rounded(md),
            style(.bg = on || ui->role_drag.id == r->id ? k->selected
                                                       : CIVUE_CLEAR),
            hover(bg(on ? k->selected : k->hover)), cursor_pointer) {
          civue_button_state state = button();

          rects[i] = here();
          drag_follow(&ui->role_drag, r->id, &state, role_movable(app, g, r));

          if (state.activated && ui->role_drag.id == 0 && !on) {
            open_role(app, ui, g, r->id, ui->role_tab);
          }

          div(size(3), flex_shrink_0, mx(1), rounded(full),
              bg(color_of(d ? d->color : r->color))) {}
          text(name, flex_1, min_w(0), truncate, text_size(px(14)),
               line_height(px(18)), font_weight(medium),
               text_color(on || everyone ? k->strong : k->text));

          if (locked && !everyone) {
            icon(lucide, lock, size(4), flex_shrink_0, ml(1),
                 text_color(k->subtle));
          }
        }
      }
    }
  }

  // @everyone stays last, out of the drag.
  roles_drop(app, ui, g, roles, rects, n > 0 ? n - 1 : 0, 0);

  if (a->role.status == ADMIN_DONE && a->role.made != 0) {
    Snowflake made = a->role.made;

    admin_clear(a, &a->role);
    open_role(app, ui, g, made, ROLE_TAB_DISPLAY);
  }
}

static void swatch(civue_id id, civue_color color, bool on, float w, float h,
                   bool pencil, bool *pressed) {
  float luminance = 0.299f * color.r + 0.587f * color.g + 0.114f * color.b;
  civue_color mark = luminance > .6f ? CIVUE_BLACK : CIVUE_WHITE;

  div(key(id), relative, flex, items_center, justify_center, flex_shrink_0,
      style(.w = civue_px(w), .h = civue_px(h)), rounded(lg), bg(color),
      text_color(mark), cursor_pointer) {
    *pressed = button().activated;

    if (on) {
      civue_node_icon(civue_ui_current(), CIVUE_ICON(lucide, check),
                      &CIVUE_NODE_STYLE(.w = civue_px(h > 30 ? 24 : 14),
                                        .h = civue_px(h > 30 ? 24 : 14)));
    }

    if (pencil) {
      icon(lucide, pencil, absolute, top(px(4)), right(px(4)), size(px(14)));
    }
  }
}

// the hsv of 0xrrggbb, each from 0 to 1.
static void to_hsv(uint32_t c, float *h, float *s, float *v) {
  float r = (float)((c >> 16) & 0xff) / 255;
  float g = (float)((c >> 8) & 0xff) / 255;
  float b = (float)(c & 0xff) / 255;
  float most = fmaxf(r, fmaxf(g, b)), least = fminf(r, fminf(g, b));
  float d = most - least;

  *v = most;
  *s = most > 0 ? d / most : 0;

  if (d == 0) {
    *h = 0;
  }
  else if (most == r) {
    *h = fmodf((g - b) / d + 6, 6) / 6;
  }
  else if (most == g) {
    *h = ((b - r) / d + 2) / 6;
  }
  else {
    *h = ((r - g) / d + 4) / 6;
  }
}

static uint32_t from_hsv(float h, float s, float v) {
  float r = 0, g = 0, b = 0;
  float f = h * 6 - floorf(h * 6);
  float p = v * (1 - s), q = v * (1 - f * s), t = v * (1 - (1 - f) * s);

  switch ((int)(h * 6) % 6) {
    case 0:
      r = v, g = t, b = p;
      break;
    case 1:
      r = q, g = v, b = p;
      break;
    case 2:
      r = p, g = v, b = t;
      break;
    case 3:
      r = p, g = q, b = v;
      break;
    case 4:
      r = t, g = p, b = v;
      break;
    default:
      r = v, g = p, b = q;
      break;
  }

  return ((uint32_t)(r * 255 + .5f) << 16) |
         ((uint32_t)(g * 255 + .5f) << 8) | (uint32_t)(b * 255 + .5f);
}

static float clamp01(double v) {
  return (float)(v < 0 ? 0 : v > 1 ? 1 : v);
}

// the popout of a custom color: saturation and value over a square, the
// hue along a bar, the hex under them. true once it should close.
static bool custom_picker(App *app, civue_rect anchor, uint32_t *color) {
  const Look *k = &app->look;
  civue_ui *cui = civue_ui_current();
  float *hsv =
    civue_memory(cui, id("role-picker-hsv"), 4 * sizeof(float), nullptr);
  bool changed = false;
  civue_rect card = {};
  char hex[8];
  char *typed = nullptr;

  // the fourth is whether the three were taken from the color yet.
  if (hsv[3] == 0) {
    to_hsv(*color ? *color : ROLE_DEFAULT_COLOR, &hsv[0], &hsv[1], &hsv[2]);
    hsv[3] = 1;
  }

  overlay() {
    div(key(id("role-picker")), flex, flex_col, gap(4), w(px(220)), p(4),
        left(px(anchor.x1 + 8)), top(px(anchor.y0)), rounded(md),
        bg(k->panel), border(1), border_color(k->border), shadow(lg)) {
      card = here();
      button();

      div(key(id("role-picker-sv")), relative, h(px(140)), rounded(lg),
          overflow_hidden, cursor_crosshair) {
        civue_button_state state = button();
        civue_rect r = here();
        int cols = 24, rows = 18;
        double cw = (r.x1 - r.x0) / cols, ch = (r.y1 - r.y0) / rows;

        for (int y = 0; y < rows; y++) {
          for (int x = 0; x < cols; x++) {
            uint32_t c = from_hsv(hsv[0], (float)x / (float)(cols - 1),
                                  1 - (float)y / (float)(rows - 1));

            fill(civue_rect_from(r.x0 + x * cw, r.y0 + y * ch,
                                 r.x0 + (x + 1) * cw + .5,
                                 r.y0 + (y + 1) * ch + .5),
                 rgb(c), 0);
          }
        }

        if (state.response.dragging || state.response.pressed) {
          hsv[1] = clamp01((cui->input.mouse.x - r.x0) / (r.x1 - r.x0));
          hsv[2] = 1 - clamp01((cui->input.mouse.y - r.y0) / (r.y1 - r.y0));
          changed = true;
        }

        double x = r.x0 + hsv[1] * (r.x1 - r.x0);
        double y = r.y0 + (1 - hsv[2]) * (r.y1 - r.y0);

        civue_stroke(cui, civue_rect_from(x - 6, y - 6, x + 6, y + 6),
                     CIVUE_WHITE, 3, 6);
      }

      div(key(id("role-picker-hue")), relative, h(px(20)), rounded(lg),
          overflow_hidden, cursor_pointer) {
        civue_button_state state = button();
        civue_rect r = here();
        int steps = 48;
        double sw = (r.x1 - r.x0) / steps;

        for (int i = 0; i < steps; i++) {
          fill(civue_rect_from(r.x0 + i * sw, r.y0, r.x0 + (i + 1) * sw + .5,
                               r.y1),
               rgb(from_hsv((float)i / (float)steps, 1, 1)), 0);
        }

        if (state.response.dragging || state.response.pressed) {
          hsv[0] = fminf(clamp01((cui->input.mouse.x - r.x0) / (r.x1 - r.x0)),
                         .999f);
          changed = true;
        }

        double x = r.x0 + hsv[0] * (r.x1 - r.x0);

        civue_stroke(cui, civue_rect_from(x - 6, r.y0 + 4, x + 6, r.y1 - 4),
                     CIVUE_WHITE, 3, 6);
      }

      snprintf(hex, sizeof hex, "%06X", from_hsv(hsv[0], hsv[1], hsv[2]));
      typed = strdup(hex);

      civue_field_response field =
        form_input(app, id("role-picker-hex"), &typed, "000000", "#", 6);

      if (field.changed && typed != nullptr && strlen(typed) == 6) {
        char *end = nullptr;
        unsigned long value = strtoul(typed, &end, 16);

        if (end != nullptr && *end == 0) {
          to_hsv((uint32_t)value, &hsv[0], &hsv[1], &hsv[2]);
          changed = true;
        }
      }

      free(typed);
    }
  }

  if (changed) {
    *color = from_hsv(hsv[0], hsv[1], hsv[2]);
  }

  bool outside = cui->input.pressed[0] && cui->input.has_mouse &&
                 !civue_rect_contains(card, cui->input.mouse) &&
                 !civue_rect_contains(anchor, cui->input.mouse);

  if (outside || civue_key_pressed(cui, CIVUE_KEY_ESCAPE)) {
    civue_forget(cui, id("role-picker-hsv"));
    return true;
  }

  return false;
}

static void display_tab(App *app, RoleDraft *d, bool editable) {
  const Look *k = &app->look;
  civue_ui *cui = civue_ui_current();
  bool *picking = civue_memory(cui, id("role-picking"), sizeof(bool), nullptr);
  civue_rect custom = {};

  div(flex, items_center, gap(1), mb(2)) {
    text("Role name", font_weight(medium), text_color(k->strong));
    text("*", text_color(k->critical_text));
  }

  if (editable) {
    form_input(app, id("role-name"), &d->name, "", nullptr, 100);
  }
  else {
    div(flex, items_center, h(10), px(3), rounded(lg), bg(k->field), border(1),
        border_color(k->border)) {
      text(d->name ? d->name : "", text_color(k->faint));
    }
  }

  form_divider(k);

  div(flex, items_center, gap(1), mb(1)) {
    text("Role color", font_weight(medium), text_color(k->strong));
    text("*", text_color(k->critical_text));
  }

  text("Members use the color of the highest role they have on the roles "
       "list.",
       flex_shrink_0, mb(4), text_size(px(14)), line_height(px(18)),
       text_color(k->subtle));

  div(flex, flex_wrap, items_start, gap(px(10)), flex_shrink_0) {
    bool pressed = false;
    bool preset = d->color == 0;

    for (size_t i = 0; i < 20; i++) {
      preset = preset || SWATCHES[i] == d->color;
    }

    swatch(id("role-color-default"), rgb(ROLE_DEFAULT_COLOR), d->color == 0, 66,
           50, false, &pressed);

    if (pressed && editable) {
      d->color = 0;
    }

    div(flex, flex_col) {
      swatch(id("role-color-custom"), preset ? k->rule : rgb(d->color), !preset,
             66, 50, true, &pressed);
      custom = here();
    }

    if (pressed && editable) {
      *picking = !*picking;
    }

    div(flex, flex_col, gap(px(10)), flex_shrink_0) {
      for (int row = 0; row < 2; row++) {
        div(flex, gap(px(10)), flex_shrink_0) {
          for (int col = 0; col < 10; col++) {
            uint32_t c = SWATCHES[row * 10 + col];

            swatch(id_at(id("role-swatch"), row * 10 + col), rgb(c),
                   d->color == c, 20, 20, false, &pressed);

            if (pressed && editable) {
              d->color = c;
            }
          }
        }
      }
    }
  }

  if (*picking && editable && custom_picker(app, custom, &d->color)) {
    *picking = false;
  }

  form_divider(k);

  if (form_switch_row(app, id("role-hoist"),
                      "Display role members separately from online members",
                      nullptr, d->hoist, !editable)) {
    d->hoist = !d->hoist;
  }

  form_divider(k);

  if (form_switch_row(app, id("role-mentionable"),
                      "Allow anyone to @mention this role",
                      "Note: Members with the \"Mention @everyone, @here, "
                      "and All Roles\" permission will always be able to "
                      "ping this role.",
                      d->mentionable, !editable)) {
    d->mentionable = !d->mentionable;
  }
}

static bool permission_matches(const ServerUi *ui, const PermissionRow *row) {
  const char *q = ui->perm_search;

  return q == nullptr || *q == 0 || strcasestr(row->title, q) ||
         strcasestr(row->about, q);
}

static void permission_row(App *app, const Guild *g, RoleDraft *d,
                           const PermissionRow *row, bool editable,
                           bool first) {
  const Look *k = &app->look;
  const char *why = editable ? locked_why(app, g, d, row->bit) : nullptr;
  bool locked = !editable || why != nullptr;
  bool on = (d->permissions & row->bit) != 0;

  div(flex, flex_col, gap(2), flex_shrink_0, pt(6), border_t(first ? 0 : 1),
      border_color(k->border)) {
    if (form_switch_row(app, id_at(id("role-perm"), row->bit), row->title,
                        row->about, on, locked)) {
      d->permissions ^= row->bit;
    }

    if (why != nullptr) {
      div(flex, items_center, gap(1), text_color(k->critical_text)) {
        icon(lucide, ban, size(px(14)), flex_shrink_0);
        text(why, text_size(px(12)), line_height(px(16)), font_weight(medium));
      }
    }
  }
}

static void permissions_tab(App *app, ServerUi *ui, Guild *g, RoleDraft *d,
                            bool editable) {
  const Look *k = &app->look;
  Permissions mine = store_guild_permissions(app->session->store, g);
  size_t shown = 0;
  // clearing may not take from the user what they have.
  bool clearable =
    editable && d->permissions != 0 &&
    (is_owner(app, g) || (mine_if(app, g, d->id, 0) & mine) == mine);

  div(flex, items_center, flex_shrink_0, mt(4), mb(8)) {
    div(flex_1, min_w(0)) {
      form_input(app, id("role-perm-search"), &ui->perm_search,
                 "Search permissions", nullptr, 0);
    }
  }

  for (size_t gi = 0; gi < sizeof GROUPS / sizeof *GROUPS; gi++) {
    const PermissionGroup *group = &GROUPS[gi];
    size_t first = shown;

    if (group->community && !g->community) {
      continue;
    }

    for (size_t i = 0; i < group->count; i++) {
      const PermissionRow *row = &group->rows[i];

      if ((row->community && !g->community) || !permission_matches(ui, row)) {
        continue;
      }

      if (shown == first) {
        div(flex, items_center, flex_shrink_0, mb(3),
            style(.margin[0] = civue_px(first ? 32 : 0))) {
          text(group->title, flex_1, text_size(px(20)), line_height(px(24)),
               font_weight(medium), text_color(k->strong));

          if (first == 0 && form_button_small(app, id("role-perm-clear"),
                                              "Clear permissions", BUTTON_LINK,
                                              !clearable)) {
            d->permissions = 0;
          }
        }
      }

      permission_row(app, g, d, row, editable, shown == first);
      shown++;

      div(flex_shrink_0, h(6)) {}
    }
  }

  if (shown == 0) {
    text("No permissions match.", mt(6), text_center, text_color(k->faint));
  }
}

static bool has_role(const Member *m, Snowflake role_id) {
  for (size_t i = 0; m != nullptr && i < m->nroles; i++) {
    if (m->roles[i] == role_id) {
      return true;
    }
  }

  return false;
}

static bool member_matches(App *app, const Guild *g, Snowflake user_id,
                           const char *q) {
  Store *store = app->session->store;
  const User *u = store_user(store, user_id);

  if (q == nullptr || *q == 0) {
    return true;
  }

  return strcasestr(store_display_name(store, g->id, user_id), q) ||
         (u && u->username && strcasestr(u->username, q));
}

static void member_row(App *app, ServerUi *ui, Guild *g, Snowflake user_id,
                       bool removable) {
  const Look *k = &app->look;
  civue_ui *cui = civue_ui_current();
  Store *store = app->session->store;
  const User *u = store_user(store, user_id);
  bool remove = false;

  div(key(id_at(id("role-member"), user_id)), flex, items_center,
      flex_shrink_0, h(px(MEMBER_ROW)), py(1), pl(2), rounded(md),
      hover(bg(k->hover)), active(bg(k->pressed))) {
    avatar(app, u, g->id, 24, false, k->panel);
    text(store_display_name(store, g->id, user_id), ml(2), min_w(0), truncate,
         text_size(px(14)), line_height(px(18)), font_weight(semibold),
         text_color(k->strong));

    if (u != nullptr && u->username != nullptr) {
      text(u->username, ml(1), min_w(0), truncate, text_size(px(14)),
           line_height(px(18)), text_color(k->subtle));
    }

    div(flex_1) {}

    div(key(id_at(id("role-member-remove"), user_id)), flex, items_center,
        justify_center, size(6), mx(2), rounded(full),
        text_color(removable ? k->subtle : k->muted),
        hover(text_color(removable ? k->strong : k->muted)),
        style(.cursor = removable ? CIVUE_CURSOR_POINTER : CIVUE_CURSOR_DEFAULT,
              .has_cursor = true)) {
      civue_button_state state = button();

      remove = state.activated && removable;
      hint(app, id_at(id("role-member-hint"), user_id), &state.response,
           removable ? "Remove member"
                     : "Members cannot be manually added or removed from "
                       "this role",
           CIVUE_TOOLTIP_ABOVE);
      icon(lucide, circle_x, size(4));
    }
  }

  if (!remove) {
    return;
  }

  // shift skips the question, as discord's.
  if (cui->input.mods & CIVUE_MOD_SHIFT) {
    admin_remove_member(app->session, g->id, ui->role_id, user_id);
  }
  else {
    ui->confirm = CONFIRM_REMOVE_MEMBER;
    ui->confirm_id = user_id;
  }
}

static void members_tab(App *app, ServerUi *ui, Guild *g, bool editable) {
  const Look *k = &app->look;
  Admin *a = admin_of(app->session, g->id);
  const Role *r = store_role(g, ui->role_id);
  bool managed = r != nullptr && role_managed(r);
  bool removable = editable && !managed;
  size_t shown = 0;
  uint32_t total = admin_count(a, ui->role_id);
  Ids listed = {};

  div(flex, items_center, gap(4), flex_shrink_0, mt(2)) {
    div(flex_1, min_w(0)) {
      civue_field_response field =
        form_input(app, id("role-member-search"), &ui->member_search,
                   "Search Members", nullptr, 0);

      if (field.changed && ui->member_search != nullptr) {
        admin_search_members(app->session, g->id, ui->member_search);
      }
    }

    if (form_button_small(app, id("role-member-add"), "Add Members",
                          BUTTON_PRIMARY, !removable)) {
      ui->picker_open = true;
      ui->picks.len = 0;
      free(ui->picker_search);
      free(ui->picker_asked);
      ui->picker_search = ui->picker_asked = nullptr;
    }
  }

  const char *q = ui->member_search;

  if (total > a->nmembers && a->members_role == ui->role_id) {
    div(flex, items_center, gap(2), flex_shrink_0, mt(4), p(3), rounded(lg),
        bg(civue_alpha(k->primary, .16f)), text_color(k->text)) {
      icon(lucide, info, size(5), flex_shrink_0);
      text("Not all members are shown, use Search to find specific members",
           text_size(px(14)), line_height(px(18)));
    }
  }

  div(flex, flex_col, flex_shrink_0, mt(4), pb(px(60))) {
    for (size_t i = 0; i < a->nmembers && a->members_role == ui->role_id;
         i++) {
      if (!member_matches(app, g, a->members[i], q)) {
        continue;
      }

      ids_toggle(&listed, a->members[i]);
      member_row(app, ui, g, a->members[i], removable);
      shown++;
    }

    // a search finds members past the hundred discord lists.
    if (q != nullptr && *q) {
      CILO_MAP_EACH(entry, &g->members) {
        const Member *m = entry->val;

        if (has_role(m, ui->role_id) && !ids_has(&listed, m->user_id) &&
            member_matches(app, g, m->user_id, q)) {
          member_row(app, ui, g, m->user_id, removable);
          shown++;
        }
      }
    }
  }

  free(listed.v);

  if (a->members_loading && shown == 0) {
    text("Loading members...", mt(4), text_color(k->subtle));
  }
  else if (shown == 0) {
    div(flex, items_center, gap(2), text_color(k->muted)) {
      icon(lucide, users, size(5));
      text("No members were found. Add members to this role.",
           text_size(px(14)), line_height(px(18)), font_weight(semibold));
    }
  }
}

static void role_editor(App *app, ServerUi *ui, Guild *g) {
  const Look *k = &app->look;
  Admin *a = admin_of(app->session, g->id);
  const Role *r = store_role(g, ui->role_id);
  bool back = false;
  char title[160], members[48];

  // the role was deleted.
  if (r == nullptr) {
    ui->role_id = 0;
    return;
  }

  RoleDraft *d = draft_for(ui, r);
  bool everyone = r->id == g->id;
  bool editable = role_editable(app, g, r);
  static const char *const TABS[] = {"Display", "Permissions",
                                     "Manage Members"};

  snprintf(title, sizeof title, "Edit Role \xc2\xb7 %s",
           everyone ? "@everyone" : d->name ? d->name : "");

  if (everyone) {
    snprintf(members, sizeof members, "Manage Members");
  }
  else {
    snprintf(members, sizeof members, "Manage Members (%u)",
             admin_count(a, r->id));
  }

  div(flex, flex_shrink_0) {
    editor_column(app, ui, g, &back);

    div(flex, flex_col, flex_1, min_w(0), pl(6)) {
      div(flex, items_center, justify_between, flex_shrink_0, mb(6)) {
        text(title, flex_1, min_w(0), truncate, font_weight(semibold),
             text_color(k->strong));

        if (!everyone) {
          div(key(id("role-editor-more")), flex, items_center, justify_center,
              size(6), ml(4), text_color(k->strong), cursor_pointer) {
            if (button().activated) {
              ui->role_menu = r->id;
              ui->role_menu_at = here();
            }

            icon(lucide, ellipsis, size(5));
          }
        }
      }

      if (!editable) {
        form_note(k, "Role is locked because it is a higher rank than your "
                     "highest role.");
        div(flex_shrink_0, h(4)) {}
      }

      div(flex, flex_shrink_0, border_b(2), border_color(k->border)) {
        for (int t = 0; t < 3; t++) {
          bool on = ui->role_tab == t;
          bool disabled = everyone && t != ROLE_TAB_PERMISSIONS;
          civue_color color = disabled ? k->muted
                              : on     ? k->accent_text
                                       : k->subtle;

          div(key(id_at(id("role-tab"), t)), relative, flex, flex_col,
              flex_shrink_0, mr(8), pb(4), text_size(px(14)),
              line_height(px(14)), font_weight(medium), text_color(color),
              style(.cursor = disabled ? CIVUE_CURSOR_DEFAULT
                                       : CIVUE_CURSOR_POINTER,
                    .has_cursor = true)) {
            civue_button_state state = button();
            bool lit = state.response.hovered && !disabled;

            if (state.activated && !disabled) {
              ui->role_tab = (RoleTab)t;
            }

            text(t == ROLE_TAB_MEMBERS ? members : TABS[t],
                 text_color(lit && !on ? k->strong : color));

            if (on || lit) {
              div(absolute, left(0), bottom(px(-2)), w(full), h(px(2)),
                  rounded_t(px(2)), bg(on ? k->accent_text : k->strong)) {}
            }
          }
        }
      }

      div(flex, flex_col, flex_shrink_0, mt(6)) {
        switch (ui->role_tab) {
          case ROLE_TAB_DISPLAY:
            display_tab(app, d, editable && !everyone);
            break;
          case ROLE_TAB_PERMISSIONS:
            permissions_tab(app, ui, g, d, editable);
            break;
          case ROLE_TAB_MEMBERS:
            members_tab(app, ui, g, editable);
            break;
        }
      }
    }
  }

  if (back) {
    ui->role_id = 0;
    admin_fetch_counts(app->session, g->id);
  }
}

void roles_page(App *app, ServerUi *ui, Guild *g) {
  if (ui->role_id != 0) {
    role_editor(app, ui, g);
  }
  else {
    roles_list(app, ui, g);
  }
}

/* ========================================================================== */
/* the menu of a role, its confirmations and the picker of members.           */
/* ========================================================================== */

static void role_menu(App *app, ServerUi *ui, Guild *g) {
  const Look *k = &app->look;
  civue_ui *cui = civue_ui_current();
  const Role *r = store_role(g, ui->role_menu);
  bool copy = false, remove = false;
  civue_rect card = {};

  if (ui->role_menu == 0) {
    return;
  }

  if (r == nullptr) {
    ui->role_menu = 0;
    return;
  }

  civue_rect room = overlay_room();
  float left = fminf((float)ui->role_menu_at.x0, (float)room.x1 - 200);
  bool editable = role_editable(app, g, r) && !role_managed(r);

  overlay() {
    div(key(id("role-menu")), flex, flex_col, w(px(200)), left(px(left)),
        top(px(ui->role_menu_at.y1 + 4)), p(2), rounded(lg), bg(k->popover),
        border(1), border_color(k->border), shadow(lg)) {
      card = here();

      copy = menu_row(app, id("role-menu-copy"), CIVUE_ICON(lucide, copy),
                      "Copy Role ID", k->text);

      if (editable) {
        div(flex_shrink_0, h(px(1)), m(2), bg(k->border)) {}
        remove = menu_row(app, id("role-menu-delete"),
                          CIVUE_ICON(lucide, trash_2), "Delete Role",
                          k->critical_text);
      }
    }
  }

  // the press that opened it is not one outside of it.
  bool outside = (cui->input.pressed[0] || cui->input.pressed[1]) &&
                 cui->input.has_mouse && card.x1 > card.x0 &&
                 !civue_rect_contains(card, cui->input.mouse) &&
                 !civue_rect_contains(ui->role_menu_at, cui->input.mouse);

  if (copy) {
    char digits[21];

    snowflake_format(r->id, digits);
    civue_clipboard_set_text(digits, strlen(digits));
  }

  if (remove) {
    Admin *a = admin_of(app->session, g->id);

    ui->confirm = CONFIRM_DELETE_ROLE;
    ui->confirm_id = r->id;
    admin_clear(a, &a->role);
  }

  if (copy || remove || outside || civue_key_pressed(cui, CIVUE_KEY_ESCAPE)) {
    ui->role_menu = 0;
  }
}

static void role_confirm(App *app, ServerUi *ui, Guild *g) {
  Admin *a = admin_of(app->session, g->id);
  char body[256];

  if (ui->confirm == CONFIRM_DELETE_ROLE) {
    const Role *r = store_role(g, ui->confirm_id);

    if (r == nullptr) {
      ui->confirm = CONFIRM_NONE;

      if (a->role.status == ADMIN_DONE) {
        admin_clear(a, &a->role);
      }

      return;
    }

    snprintf(body, sizeof body,
             "Are you sure you want to delete the %s role? This action "
             "cannot be undone.",
             r->name ? r->name : "");

    int answer = form_confirm(app, id("confirm-delete-role"), "Delete Role",
                              body, "Delete", true,
                              a->role.status == ADMIN_BUSY, a->role.error);

    if (answer > 0) {
      admin_delete_role(app->session, g->id, r->id);
    }
    else if (answer < 0) {
      ui->confirm = CONFIRM_NONE;
      admin_clear(a, &a->role);
    }
  }
  else if (ui->confirm == CONFIRM_REMOVE_MEMBER) {
    const Role *r = store_role(g, ui->role_id);
    const User *u = store_user(app->session->store, ui->confirm_id);

    snprintf(body, sizeof body,
             "Remove %s from role %s?\n\nHold shift when removing members to "
             "skip this modal.",
             u && u->username ? u->username : "this member",
             r && r->name ? r->name : "");

    int answer = form_confirm(app, id("confirm-remove-member"),
                              "Remove member", body, "Remove", true, false,
                              nullptr);

    if (answer > 0 && r != nullptr) {
      admin_remove_member(app->session, g->id, r->id, ui->confirm_id);
    }

    if (answer != 0) {
      ui->confirm = CONFIRM_NONE;
    }
  }
}

static void member_picker(App *app, ServerUi *ui, Guild *g) {
  const Look *k = &app->look;
  civue_ui *cui = civue_ui_current();
  Store *store = app->session->store;
  const Role *r = store_role(g, ui->role_id);
  bool close = false, add = false;
  size_t shown = 0;
  char subtitle[200];

  if (!ui->picker_open) {
    return;
  }

  if (r == nullptr) {
    ui->picker_open = false;
    return;
  }

  // discord asks for the members whose names start as typed.
  const char *q = ui->picker_search ? ui->picker_search : "";

  if (ui->picker_asked == nullptr || strcmp(ui->picker_asked, q) != 0) {
    str_set(&ui->picker_asked, q);
    admin_search_members(app->session, g->id, q);
  }

  snprintf(subtitle, sizeof subtitle,
           "Select up to 30 members to add to role %s", r->name ? r->name : "");

  overlay() {
    div(key(id("picker-shade")), left(0), top(0), w(px(cui->size.x)),
        h(px(cui->size.y)), flex, items_center, justify_center, bg(k->scrim)) {
      close = button().activated;

      // a header, a list that scrolls and a foot, each 24 from the edges.
      div(key(id("picker")), flex, flex_col, w(px(dialog_width())),
          h(px(560)), rounded(px(DIALOG_RADIUS)), bg(k->dialog), border(1),
          border_color(k->border), shadow(lg), text_color(k->text),
          text_size(px(14)), line_height(px(20)), overflow_hidden) {
        button();

        div(flex, flex_col, gap(4), flex_shrink_0, px(6), pt(6), pb(4)) {
          dialog_header(k, "Add members", subtitle);
          form_input(app, id("picker-search"), &ui->picker_search,
                     "Search members", nullptr, 0);
        }

        div(key(id("picker-list")), flex, flex_col, flex_1, min_h(0),
            overflow_y_scroll, scrollbars(&app->thin_bars), px(5)) {
          text("Members", flex_shrink_0, pt(2), h(8), text_size(px(14)),
               font_weight(semibold), text_color(k->text));

          CILO_MAP_EACH(entry, &g->members) {
            const Member *m = entry->val;
            bool picked = ids_has(&ui->picks, m->user_id);

            if (has_role(m, r->id) ||
                !member_matches(app, g, m->user_id, ui->picker_search)) {
              continue;
            }

            if (++shown > 200) {
              break;
            }

            const User *u = store_user(store, m->user_id);

            div(key(id_at(id("picker-row"), m->user_id)), flex, items_center,
                flex_shrink_0, h(px(40)), px(px(6)), rounded(md),
                style(.bg = picked ? k->selected : CIVUE_CLEAR),
                hover(bg(k->selected)), cursor_pointer) {
              bool flip = button().activated;

              avatar(app, u, g->id, 24, false, k->dialog);
              text(store_display_name(store, g->id, m->user_id), ml(2), mr(1),
                   min_w(0), truncate, text_size(px(14)), line_height(px(18)),
                   text_color(k->strong));

              if (u != nullptr && u->username != nullptr) {
                text(u->username, min_w(0), truncate, text_size(px(14)),
                     line_height(px(18)), text_color(k->muted));
              }

              div(flex_1) {}
              text("Member", text_size(px(12)), line_height(px(16)),
                   text_color(k->muted));

              div(flex, pl(2)) {
                flip |= form_checkbox(
                  app, id_at(id("picker-check"), m->user_id), picked);
              }

              if (flip) {
                ids_toggle(&ui->picks, m->user_id);
              }
            }
          }

          if (shown == 0) {
            text("No members found. Try another name.", mt(6), text_center,
                 text_color(k->faint));
          }
        }

        div(flex, justify_end, gap(2), flex_shrink_0, px(6), pt(4), pb(6)) {
          close |= form_button(app, id("picker-cancel"), "Cancel",
                               BUTTON_OUTLINE, false);
          add = form_button(app, id("picker-add"), "Add", BUTTON_PRIMARY,
                            ui->picks.len == 0 || ui->picks.len > ADD_MOST);
        }
      }
    }
  }

  if (civue_key_pressed(cui, CIVUE_KEY_ESCAPE)) {
    close = true;
  }

  if (add && ui->picks.len <= ADD_MOST) {
    admin_add_members(app->session, g->id, r->id, ui->picks.v, ui->picks.len);
    close = true;
  }

  if (close) {
    ui->picker_open = false;
    ui->picks.len = 0;
  }
}

void roles_dialogs(App *app, ServerUi *ui, Guild *g) {
  role_menu(app, ui, g);
  role_confirm(app, ui, g);
  member_picker(app, ui, g);
}
