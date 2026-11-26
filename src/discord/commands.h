#ifndef QUARREL_DISCORD_COMMANDS_H
#define QUARREL_DISCORD_COMMANDS_H

#include "discord/session.h"

/* ========================================================================== */
/* slash commands: the ones discord's client has of its own, then the ones of */
/* the applications of a guild, of a private channel and of the user's        */
/* account, as discord's command indexes list them. an index is asked for the */
/* first time its commands are, and again once discord says it changed.       */
/*                                                                            */
/*   size_t n;                                                                */
/*   const Slash *slashes = commands_in(session, channel, &n);                */
/*   ... the user picks slashes[i] and types its options ...                  */
/*   commands_run(session, channel->id, &slashes[i], values, nvalues);        */
/*                                                                            */
/* an application's command is an interaction: discord answers through the    */
/* gateway, with the message of the application or with a failure, and the    */
/* store shows a pending message meanwhile.                                   */
/* ========================================================================== */

typedef enum : uint8_t {
  OPTION_SUB_COMMAND = 1,
  OPTION_SUB_COMMAND_GROUP = 2,
  OPTION_STRING = 3,
  OPTION_INTEGER = 4,
  OPTION_BOOLEAN = 5,
  OPTION_USER = 6,
  OPTION_CHANNEL = 7,
  OPTION_ROLE = 8,
  OPTION_MENTIONABLE = 9,
  OPTION_NUMBER = 10,
  OPTION_ATTACHMENT = 11,
} CommandOptionType;

// one of the values an option takes when it lists them.
typedef struct {
  // what is shown, in the user's language when discord has it.
  char *name;
  // what is sent: a string, or a number as json writes one when `number`.
  char *value;
  bool number;
} CommandChoice;

typedef struct CommandOption CommandOption;

struct CommandOption {
  // a CommandOptionType, or a number this build does not know.
  uint8_t type;
  // `name` is what discord knows it by and `shown` what the user reads and
  // types, the same unless the application translated it.
  char *name, *shown, *description;
  bool required;
  // its application completes it while it is typed, see commands_suggest.
  bool autocomplete;
  CommandChoice *choices;
  size_t nchoices;
  // the options of a subcommand, the subcommands of a group.
  CommandOption *options;
  size_t noptions;
  // the ChannelTypes a channel option takes, any when it names none.
  uint8_t *channel_types;
  size_t nchannel_types;
  // the bounds of a number, and of a string's length in utf-16 units.
  bool has_min, has_max, has_min_length, has_max_length;
  double min_value, max_value;
  uint32_t min_length, max_length;
};

// the commands of discord's client that Quarrel has, none for an
// application's.
typedef enum : uint8_t {
  BUILTIN_NONE,
  BUILTIN_GIF,
  BUILTIN_SHRUG,
  BUILTIN_TABLEFLIP,
  BUILTIN_UNFLIP,
  BUILTIN_TTS,
  BUILTIN_ME,
  BUILTIN_SPOILER,
  BUILTIN_NICK,
  BUILTIN_THREAD,
  BUILTIN_KICK,
  BUILTIN_BAN,
  BUILTIN_TIMEOUT,
  BUILTIN_MSG,
} Builtin;

// the application discord's own commands are listed under, its "-1".
constexpr Snowflake COMMANDS_BUILT_IN = UINT64_MAX;

typedef struct {
  Snowflake id, application_id;
  // as for an option.
  char *name, *shown, *description;
  CommandOption *options;
  size_t noptions;
  Builtin builtin;

  // the commands' own: the guild of a command made for one, 0 for any.
  Snowflake guild_id, version;
  // the object as discord's client keeps it, which running the command
  // sends back.
  char *json;
} Command;

typedef struct {
  Snowflake id;
  // its bot's username, as discord's client lists it, else its own name.
  char *name;
  char *description, *icon;
  // the user its messages are written by, 0 when it has none.
  Snowflake bot_id;
} CommandApplication;

// a command as it is typed after the slash: one without subcommands, or one
// subcommand of one.
typedef struct {
  const Command *command;
  // the group and the subcommand of it this is, null for none.
  const CommandOption *group, *sub;
  // the words after the slash as they are read and typed, like "ban" or
  // "settings channel set".
  char *name;
  // never null.
  const char *description;
  // what it takes: the command's options, or the subcommand's.
  const CommandOption *options;
  size_t noptions;
} Slash;

// the commands the current user may run in a channel: each application's
// together, the applications by name, discord's own last. none for null. a
// cache good until `session->version` changes, which an index that arrives
// bumps.
const Slash *commands_in(Session *s, const Channel *c, size_t *count);
// how well a command answers what was typed after the slash, after the
// scores of discord's client: 0 for a name that starts with it, up to 12,
// lower being better. -1 for one that does not answer. `query` is in lower
// case.
int commands_score(Session *s, const Slash *slash, const char *query);
// an index the channel's commands come from is on its way.
bool commands_loading(Session *s, const Channel *c);
// the application of a command, "Built-In" for discord's own. null when
// unknown.
const CommandApplication *commands_application(Session *s,
                                               Snowflake application_id);

// what the user typed for an option.
typedef struct {
  const CommandOption *option;
  // a string as it is, a number in digits, "true" or "false", or the id of
  // a user, a channel or a role in digits.
  const char *text;
  // the option being typed, for commands_suggest.
  bool focused;
} CommandValue;

// runs a command in a channel with what its options were given. one of
// discord's own acts at once, and tells what it did through
// `session->notice`. an application's shows as a pending message until its
// answer or a failure comes. BUILTIN_GIF is the ui's to open.
void commands_run(Session *s, Snowflake channel_id, const Slash *slash,
                  const CommandValue *values, size_t count);

// asks the application of `slash` what completes the focused value, the
// others given as they are so far. the same question again asks nothing.
void commands_suggest(Session *s, Snowflake channel_id, const Slash *slash,
                      const CommandValue *values, size_t count);
// what it answered to the last question, none while `loading`, and when it
// never did, which `failed` tells.
const CommandChoice *commands_suggestions(const Session *s, size_t *count,
                                          bool *loading, bool *failed);

// presses a button of a message, or gives a menu the `values` picked in it.
// its application answers through the gateway, by changing the message, by
// another one or by nothing, and a failure is told through
// `session->notice`. nothing for a link button, which the ui opens.
void commands_component(Session *s, const Message *m, const Component *c,
                        const char *const *values, size_t count);
// the component was pressed and its application has not answered yet.
[[gnu::pure]] bool commands_component_busy(const Session *s,
                                           Snowflake message_id,
                                           const char *custom_id);

/* ========================================================================== */
/* for session.c.                                                             */
/* ========================================================================== */

// the dispatches of interactions and of command indexes.
void commands_dispatch(Session *s, const char *type, const cima_json *data);
// forgets every index and what is on its way, for another account.
void commands_clear(Session *s);
void commands_free(struct Commands *commands);

#endif
