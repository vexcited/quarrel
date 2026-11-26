#ifndef QUARREL_DISCORD_EXPRESSIONS_H
#define QUARREL_DISCORD_EXPRESSIONS_H

#include "discord/store.h"

/* ========================================================================== */
/* the emojis of the user's guilds: who may use which, under what name, and   */
/* how a message names them.                                                  */
/*                                                                            */
/*   size_t n;                                                                */
/*   const GuildEmoji *all = guild_emojis(store, channel->guild_id, &n);      */
/*   if (guild_emoji_access(store, all[0].guild, all[0].emoji, channel) ==    */
/*       EMOJI_USABLE) ...                                                    */
/* ========================================================================== */

// whether the current user may use a guild's emoji somewhere, as the client
// decides it.
typedef enum : uint8_t {
  EMOJI_USABLE,
  // with nitro only: one of another guild, or one that moves.
  EMOJI_NEEDS_PREMIUM,
  // once its guild has the boosts its slot came with back.
  EMOJI_NEEDS_BOOSTS,
  // for roles the user has none of, or of another guild where the channel
  // takes none from elsewhere. the client never shows these.
  EMOJI_HIDDEN,
} EmojiAccess;

// `c` is where the emoji would go, null for nowhere in particular.
EmojiAccess guild_emoji_access(Store *s, const Guild *owner, const Emoji *e,
                               const Channel *c);
// whether discord takes the emoji as it is in a message to `c`, the
// canUseEmote of Vencord's FakeNitro. a message holds any other as a link to
// its picture, so every emoji can go in one.
bool guild_emoji_native(Store *s, const Guild *owner, const Emoji *e,
                        const Channel *c);
// the guild of the user's an emoji is from, null for none.
const Guild *guild_emoji_owner(Store *s, Snowflake emoji_id);

typedef struct {
  const Guild *guild;
  const Emoji *emoji;
  // what it is written between colons: its own name, with ~1, ~2 after it
  // when an emoji before it, or one of unicode, has the same.
  const char *name;
} GuildEmoji;

// every emoji the user's roles let them use, as they are named where the
// guild `guild_id` is, 0 outside any: that guild's first, then the others'
// in the order of the guild bar, each guild's by name. good until the store
// changes.
const GuildEmoji *guild_emojis(Store *s, Snowflake guild_id, size_t *count);
// one of them by the `len` bytes of its name there, null for none.
const GuildEmoji *guild_emoji_by_name(Store *s, Snowflake guild_id,
                                      const char *name, size_t len);
const GuildEmoji *guild_emoji_by_id(Store *s, Snowflake guild_id,
                                    Snowflake emoji_id);

// how many bytes at the start of `text` are one emoji as a message holds
// it, <:name:id> or <a:name:id>, 0 when it starts with none.
size_t emoji_token(const char *text, size_t len, Snowflake *id, bool *animated,
                   const char **name, size_t *name_len);
// how many bytes at the start of `text` are a name between colons, both
// colons counted, 0 when it starts with none. a skin tone after it, as
// :wave::skin-tone-2:, is part of it and told in `tone`, 0 for none.
size_t emoji_colons(const char *text, size_t len, const char **name,
                    size_t *name_len, uint8_t *tone);

// how many bytes at the start of `text` a message keeps as they are, where
// no name between colons is an emoji: what a backslash escapes, code, a
// link, or an emoji already as a message holds it. 0 for none.
size_t expressions_literal(const char *text, size_t len);

// `text` as a message sent to the channel holds it, from malloc: a name
// between colons becomes the emoji it names there, <:name:id> for one of a
// guild the user may use and its character for one of unicode. code, links
// and what a backslash escapes stay as they are.
[[gnu::malloc, gnu::returns_nonnull]] char *
expressions_encode(Store *s, const Channel *c, const char *text);
// the same for a message, as FakeNitro sends it: every emoji of the user's
// guilds can be named, and one discord would refuse there becomes
// [name](the url of its picture), which `faked` then tells.
[[gnu::malloc, gnu::returns_nonnull]] char *
expressions_send(Store *s, const Channel *c, const char *text, bool *faked);

// whether `url` is the picture of an emoji, as FakeNitro reads the links it
// sends back. `name` is what the url says it is called, null when it says
// nothing.
bool fake_emoji_url(const char *url, Snowflake *id, bool *animated,
                    const char **name, size_t *name_len);
// whether a message of this content is nothing but one such link, bare or
// as [text](url). FakeNitro shows it as the emoji, without its embed.
bool fake_emoji_message(const char *content);

#endif
