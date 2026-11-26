#ifndef QUARREL_DISCORD_FAKESTICKERS_H
#define QUARREL_DISCORD_FAKESTICKERS_H

#include "discord/store.h"

/* ========================================================================== */
/* the stickers discord would refuse, sent and read back as Vencord's         */
/* FakeNitro does: a link to the sticker's picture, or a gif of it for an     */
/* animated png, which is shown again as the sticker it stands for.           */
/*                                                                            */
/*   if (!sticker_native(store, guild, sticker, channel)) {                   */
/*     char *said = fake_sticker_content(written, sticker);                   */
/*     ...                                                                    */
/*   }                                                                        */
/* ========================================================================== */

// the side of the picture a fake sticker is, the plugin's stickerSize.
constexpr uint32_t FAKE_STICKER_SIZE = 160;

// whether discord takes the sticker as it is in a message to `c`, the
// check of FakeNitro before it sends one.
bool sticker_native(Store *s, const Guild *owner, const GuildSticker *sticker,
                    const Channel *c);

// `content` with the link FakeNitro puts in place of the sticker after it:
// [name](the url of its picture, with its name and lossless). from malloc.
[[gnu::malloc, gnu::returns_nonnull]] char *
fake_sticker_content(const char *content, const GuildSticker *sticker);
// the animated png FakeNitro makes a gif of.
void fake_sticker_source(char *out, size_t cap, Snowflake sticker_id);
// the name it gives that gif, which is how it is known again.
void fake_sticker_filename(char *out, size_t cap, Snowflake sticker_id);

typedef struct {
  Snowflake id;
  // a StickerFormat of cdn.h.
  uint8_t format;
  char name[128];
} FakeSticker;

// whether the words of a message are nothing but a link to a sticker, bare
// or as [text](url). the sticker is shown then, not the link.
bool fake_sticker_message(const Store *s, const char *content,
                          FakeSticker *out);
// whether a file is the gif of a sticker of the user's guilds. nothing
// tells such a gif from another but its name, so only those count.
bool fake_sticker_attachment(const Store *s, const Attachment *a,
                             FakeSticker *out);
// whether an embed is the picture of the sticker the message links to,
// which the sticker stands for already.
bool fake_sticker_embed(const Store *s, const Message *m, const Embed *e);
// a message has ten files at most, and one link.
constexpr size_t FAKE_STICKERS_MAX = 11;

// every sticker read back from a message: the one its words link to, then
// its gifs. answers how many, `out` taking at most `cap`.
size_t fake_stickers(const Store *s, const Message *m, FakeSticker *out,
                     size_t cap);

// frames of premultiplied rgba8, each `width` by `height`, as a gif that
// loops: a palette for each frame and what is see-through left so. from
// malloc, null when there is no memory for it.
[[gnu::malloc]] uint8_t *fake_sticker_gif(const uint8_t *rgba,
                                          const float *delays, uint32_t width,
                                          uint32_t height, uint32_t count,
                                          size_t *len);

#endif
