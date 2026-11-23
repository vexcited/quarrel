#ifndef QUARREL_DISCORD_MARKDOWN_H
#define QUARREL_DISCORD_MARKDOWN_H

#include "discord/util.h"

/* ========================================================================== */
/* discord's markdown as a tree the ui lays out itself.                       */
/*                                                                            */
/*   MdDoc *doc = md_parse(message->content, strlen(message->content), 0);    */
/*   for (MdNode *block = doc->root->first; block; block = block->next) ...   */
/*   md_free(doc);                                                            */
/*                                                                            */
/* the root holds blocks: paragraphs, headings, subtexts, quotes, lists and   */
/* code blocks. quotes hold blocks, lists hold items, and items hold inline   */
/* nodes followed by the lists nested in them. everything else holds inline   */
/* nodes: texts, line breaks, styles holding more inline nodes, links,        */
/* mentions, emoji and timestamps. mentions only carry ids, the ui resolves   */
/* them to names.                                                             */
/* ========================================================================== */

typedef enum : uint8_t {
  MD_ROOT,

  MD_PARAGRAPH,
  // `level` from 1 to 3.
  MD_HEADING,
  MD_SUBTEXT,
  MD_QUOTE,
  // `ordered`, and `start` the number of its first item.
  MD_LIST,
  MD_LIST_ITEM,
  // `text` is the code, `lang` its language or null. a block wherever it is
  // found, usually under the root but under a style when written in one.
  MD_CODE_BLOCK,

  // `text` as it is drawn, escapes resolved.
  MD_TEXT,
  // the end of a line inside a block. two in a row are an empty line.
  MD_LINE_BREAK,
  MD_BOLD,
  MD_ITALIC,
  MD_UNDERLINE,
  MD_STRIKETHROUGH,
  MD_SPOILER,
  // inline code, `text` drawn as it is.
  MD_CODE,
  // `text` is the url, the children what is shown for it.
  MD_LINK,

  // `id` of the user, the role or the channel.
  MD_MENTION_USER,
  MD_MENTION_ROLE,
  MD_MENTION_CHANNEL,
  MD_MENTION_EVERYONE,
  MD_MENTION_HERE,

  // one unicode emoji, `text` its utf-8, a whole sequence like a flag or a
  // family being one node.
  MD_EMOJI,
  // `id` and `animated`, `text` its name without the colons.
  MD_CUSTOM_EMOJI,
  // `time` in unix seconds and `style`, `text` already formatted in the
  // local time zone.
  MD_TIMESTAMP,
} MdKind;

typedef struct MdNode MdNode;

struct MdNode {
  MdKind kind;
  uint8_t level;
  bool ordered, animated;
  // one of t, T, d, D, f, F, s, S and R.
  char style;
  uint32_t start;
  // the bytes of `text`.
  uint32_t len;
  // nul terminated, null for kinds that have none.
  const char *text;
  const char *lang;
  Snowflake id;
  int64_t time;
  MdNode *first, *last, *next;
};

typedef struct {
  MdNode *root;
  // the message is nothing but emoji and whitespace, 30 of them at most, so
  // they are drawn larger.
  bool jumbo;
  // the emoji in the whole tree, custom ones included.
  uint32_t emoji_count;
  cilo_arena arena;
} MdDoc;

typedef struct {
  // only inline nodes under one paragraph, for a topic, an embed's title or
  // the preview of a reply.
  bool inline_only;
  // unix seconds relative timestamps count from, the clock's when 0.
  int64_t now;
} MdOptions;

// never null. `options` may be.
[[gnu::returns_nonnull]] MdDoc *md_parse(const char *text, size_t len,
                                         const MdOptions *options);
void md_free(MdDoc *doc);

// the text of a tree without any markup, for a notification or a one line
// preview: mentions read as <@id>, custom emoji as :name:. from malloc.
[[gnu::malloc, gnu::returns_nonnull]] char *md_plain_text(const MdDoc *doc);

#endif
