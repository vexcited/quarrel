#include "discord/fakestickers.h"

#include <inttypes.h>
#include <stdcountof.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cilo/cilo.h>

#include "discord/cdn.h"

static void *xmalloc(size_t size) {
  void *p = malloc(size != 0 ? size : 1);

  if (p == nullptr) {
    cilo_oom(size);
  }

  return p;
}

/* ========================================================================== */
/* sending.                                                                   */
/* ========================================================================== */

// the plugin's canUseStickers is a nitro above the classic one, and a dm
// has every permission for it.
bool sticker_native(Store *s, const Guild *owner, const GuildSticker *sticker,
                    const Channel *c) {
  if (!sticker->available) {
    return false;
  }

  if (c->guild_id != 0 && c->guild_id == owner->id) {
    return true;
  }

  return store_stickers_anywhere(s) &&
         (c->guild_id == 0 || store_can(s, c, PERM_USE_EXTERNAL_STICKERS));
}

static bool spaces(char c) {
  return c == ' ' || c == '\n' || c == '\t' || c == '\r' || c == '\f' ||
         c == '\v';
}

// a value as URLSearchParams writes it.
static size_t put_query(char *out, const char *value) {
  static const char HEX[] = "0123456789ABCDEF";
  size_t n = 0;

  for (const unsigned char *at = (const unsigned char *)value; *at; at++) {
    bool plain = (*at >= '0' && *at <= '9') || (*at >= 'a' && *at <= 'z') ||
                 (*at >= 'A' && *at <= 'Z') || *at == '*' || *at == '-' ||
                 *at == '.' || *at == '_';

    if (plain) {
      out[n++] = (char)*at;
    }
    else if (*at == ' ') {
      out[n++] = '+';
    }
    else {
      out[n++] = '%';
      out[n++] = HEX[*at >> 4];
      out[n++] = HEX[*at & 15];
    }
  }

  out[n] = 0;
  return n;
}

char *fake_sticker_content(const char *content, const GuildSticker *sticker) {
  const char *name = sticker->name ? sticker->name : "";
  size_t len = strlen(content), named = strlen(name);
  // its getWordBoundary: the link stands apart from the words before it.
  bool apart = len != 0 && !spaces(content[len - 1]);
  char *query = xmalloc(named * 3 + 1);
  size_t cap = len + named * 4 + 128;
  char *out = xmalloc(cap);

  put_query(query, name);
  snprintf(out, cap,
           "%s%s[%s](https://media.discordapp.net/stickers/%" PRIu64
           ".%s?size=%u&name=%s&lossless=true)",
           content, apart ? " " : "", name, sticker->id,
           sticker->format == STICKER_GIF ? "gif" : "png", FAKE_STICKER_SIZE,
           query);
  free(query);
  return out;
}

void fake_sticker_source(char *out, size_t cap, Snowflake sticker_id) {
  snprintf(out, cap,
           "https://media.discordapp.net/stickers/%" PRIu64 ".png?size=%u",
           sticker_id, FAKE_STICKER_SIZE);
}

void fake_sticker_filename(char *out, size_t cap, Snowflake sticker_id) {
  snprintf(out, cap, "%" PRIu64 ".gif", sticker_id);
}

/* ========================================================================== */
/* reading back.                                                              */
/* ========================================================================== */

static bool starts(const char *text, const char *with) {
  return strncmp(text, with, strlen(with)) == 0;
}

static const char *digits_end(const char *at) {
  while (*at >= '0' && *at <= '9') {
    at++;
  }

  return at;
}

// its /\/stickers\/(\d+?)\./, `after` the dot.
static bool sticker_in(const char *text, Snowflake *id, const char **after) {
  static const char PATH[] = "/stickers/";

  for (const char *at = text; (at = strstr(at, PATH)) != nullptr; at++) {
    const char *digits = at + sizeof PATH - 1, *end = digits_end(digits);

    if (end != digits && *end == '.' &&
        (*id = snowflake_parse_n(digits, (size_t)(end - digits))) != 0) {
      *after = end + 1;
      return true;
    }
  }

  return false;
}

// its /\/attachments\/\d+?\/\d+?\/(\d+?)\.gif/.
static bool gif_in(const char *text, Snowflake *id) {
  static const char PATH[] = "/attachments/";

  for (const char *at = text; (at = strstr(at, PATH)) != nullptr; at++) {
    const char *channel = at + sizeof PATH - 1, *end = digits_end(channel);
    const char *file = *end == '/' ? end + 1 : nullptr;
    const char *file_end = file ? digits_end(file) : nullptr;
    const char *name = file_end && *file_end == '/' ? file_end + 1 : nullptr;
    const char *name_end = name ? digits_end(name) : nullptr;

    if (end != channel && file_end != file && name && name_end != name &&
        starts(name_end, ".gif") &&
        (*id = snowflake_parse_n(name, (size_t)(name_end - name))) != 0) {
      return true;
    }
  }

  return false;
}

// its hyperLinkRegex, /\[.+?\]\((https?:\/\/.+?)\)/, anywhere in a line.
static bool hyperlinked(const char *text) {
  for (const char *at = text + 1; (at = strstr(at + 1, "](http")) != nullptr;) {
    const char *url = at + 6 + (at[6] == 's');

    if (starts(url, "://") && url[3] != '\0' && strchr(url + 4, ')') &&
        memchr(text, '[', (size_t)(at - text - 1))) {
      return true;
    }
  }

  return false;
}

// what the plugin reads a sticker from in the words of a message: a word
// alone that is a link, or [text](link) and nothing else.
static bool lone_link(const char *content) {
  size_t len = strlen(content);

  if (len < 2) {
    return false;
  }

  if (strpbrk(content, " \n\t\r\f\v") == nullptr) {
    return starts(content, "http") || hyperlinked(content);
  }

  // its hyperLinkOnlyRegex, where a dot is no end of line.
  return content[0] == '[' && content[len - 1] == ')' &&
         strpbrk(content, "\n\r") == nullptr && hyperlinked(content);
}

// the name a url says its sticker has, as URLSearchParams reads it.
static bool name_in(const char *url, char *out, size_t cap) {
  const char *query = strchr(url, '?');
  size_t n = 0;

  for (const char *q = query ? query + 1 : nullptr; q && *q && *q != '#';) {
    size_t len = strcspn(q, "&#)");

    if (!starts(q, "name=")) {
      q += len + (q[len] == '&');
      continue;
    }

    for (size_t i = 5; i < len && n + 1 < cap; i++) {
      unsigned int byte = 0;

      if (q[i] == '%' && i + 2 < len && sscanf(q + i + 1, "%2x", &byte) == 1) {
        out[n++] = (char)byte;
        i += 2;
      }
      else {
        out[n++] = q[i] == '+' ? ' ' : q[i];
      }
    }

    break;
  }

  out[n] = 0;
  return n != 0;
}

static void named(FakeSticker *out, const GuildSticker *known,
                  const char *url) {
  if (known != nullptr && known->name != nullptr) {
    snprintf(out->name, sizeof out->name, "%s", known->name);
  }
  else if (url == nullptr || !name_in(url, out->name, sizeof out->name)) {
    snprintf(out->name, sizeof out->name, "FakeNitroSticker");
  }
}

bool fake_sticker_message(const Store *s, const char *content,
                          FakeSticker *out) {
  const char *after = nullptr;
  Snowflake id = 0;

  if (strstr(content, "/stickers/") == nullptr &&
      strstr(content, "/attachments/") == nullptr) {
    return false;
  }

  if (!lone_link(content)) {
    return false;
  }

  if (sticker_in(content, &id, &after)) {
    const GuildSticker *known = store_sticker(s, id, nullptr);

    // the plugin shows every one as a still. the sticker's own format, or
    // the one its link is in, shows the ones that move too.
    *out = (FakeSticker){
      .id = id,
      .format = known != nullptr       ? known->format
                : starts(after, "gif") ? STICKER_GIF
                                       : STICKER_PNG,
    };
    named(out, known, after);
    return true;
  }

  const GuildSticker *known =
    gif_in(content, &id) ? store_sticker(s, id, nullptr) : nullptr;

  if (known == nullptr) {
    return false;
  }

  *out = (FakeSticker){.id = id, .format = known->format};
  named(out, known, nullptr);
  return true;
}

bool fake_sticker_attachment(const Store *s, const Attachment *a,
                             FakeSticker *out) {
  Snowflake id = 0;

  if (a->content_type == nullptr || strcmp(a->content_type, "image/gif") != 0 ||
      a->url == nullptr || !gif_in(a->url, &id)) {
    return false;
  }

  const GuildSticker *known = store_sticker(s, id, nullptr);

  if (known == nullptr) {
    return false;
  }

  *out = (FakeSticker){.id = id, .format = known->format};
  named(out, known, nullptr);
  return true;
}

bool fake_sticker_embed(const Store *s, const Message *m, const Embed *e) {
  const char *url = e->url         ? e->url
                    : e->image.url ? e->image.url
                                   : e->thumbnail.url;
  const char *after = nullptr;
  FakeSticker shown;
  Snowflake id = 0;

  if (url == nullptr || e->type == nullptr || strcmp(e->type, "image") != 0 ||
      !fake_sticker_message(s, m->content, &shown)) {
    return false;
  }

  return sticker_in(url, &id, &after) ||
         (gif_in(url, &id) && store_sticker(s, id, nullptr) != nullptr);
}

size_t fake_stickers(const Store *s, const Message *m, FakeSticker *out,
                     size_t cap) {
  size_t n = 0;

  if (n < cap && fake_sticker_message(s, m->content, &out[n])) {
    n++;
  }

  for (size_t i = 0; i < m->nattachments && n < cap; i++) {
    if (fake_sticker_attachment(s, &m->attachments[i], &out[n])) {
      n++;
    }
  }

  return n;
}

/* ========================================================================== */
/* the gif of an animated sticker.                                            */
/* ========================================================================== */

typedef struct {
  uint8_t *v;
  size_t len, cap;
  bool failed;
} Bytes;

static void put(Bytes *b, const void *bytes, size_t n) {
  if (b->failed) {
    return;
  }

  if (b->len + n > b->cap) {
    size_t cap = b->cap ? b->cap * 2 : 1u << 16;

    while (cap < b->len + n) {
      cap *= 2;
    }

    uint8_t *grown = realloc(b->v, cap);

    if (grown == nullptr) {
      b->failed = true;
      return;
    }

    b->v = grown;
    b->cap = cap;
  }

  memcpy(b->v + b->len, bytes, n);
  b->len += n;
}

static void put8(Bytes *b, uint8_t v) {
  put(b, &v, 1);
}

static void put16(Bytes *b, uint32_t v) {
  put(b, (uint8_t[]){(uint8_t)v, (uint8_t)(v >> 8)}, 2);
}

// colors are counted five bits a channel, which is what tells them apart
// in a palette of 255.
constexpr uint32_t BINS = 1u << 15;
constexpr uint32_t COLORS = 255;
constexpr uint16_t CLEAR = 0xffff;

typedef struct {
  uint32_t count, sum[3];
} Bin;

typedef struct {
  // its bins are `used[from]` up to `used[to]`.
  uint32_t from, to;
  uint64_t count;
  // the channel its colors differ the most in, and by how much.
  uint8_t axis, range;
} Box;

typedef struct {
  Bin *bins;
  uint16_t *used, *sorted, *of;
  uint8_t *lut;
  uint32_t nused;
  Box boxes[COLORS];
  uint32_t nboxes;
} Quantizer;

static uint32_t channel(uint16_t bin, uint32_t axis) {
  return (bin >> (10 - 5 * axis)) & 31;
}

// a box's bins in the order of one channel, which has 32 values: counted
// rather than compared.
static void order(Quantizer *q, const Box *box) {
  uint32_t starts[33] = {};
  uint32_t n = box->to - box->from;
  uint16_t *bins = q->used + box->from;

  for (uint32_t i = 0; i < n; i++) {
    starts[channel(bins[i], box->axis) + 1]++;
  }

  for (uint32_t v = 1; v < 33; v++) {
    starts[v] += starts[v - 1];
  }

  for (uint32_t i = 0; i < n; i++) {
    q->sorted[starts[channel(bins[i], box->axis)]++] = bins[i];
  }

  memcpy(bins, q->sorted, n * sizeof *bins);
}

static void measure(const Quantizer *q, Box *box) {
  uint32_t low[3] = {31, 31, 31}, high[3] = {};

  box->count = 0;
  box->axis = 0;
  box->range = 0;

  for (uint32_t i = box->from; i < box->to; i++) {
    uint16_t bin = q->used[i];

    box->count += q->bins[bin].count;

    for (uint32_t k = 0; k < 3; k++) {
      uint32_t v = channel(bin, k);

      low[k] = v < low[k] ? v : low[k];
      high[k] = v > high[k] ? v : high[k];
    }
  }

  for (uint32_t k = 0; k < 3; k++) {
    if (high[k] - low[k] > box->range) {
      box->range = (uint8_t)(high[k] - low[k]);
      box->axis = (uint8_t)k;
    }
  }
}

// the median cut: the box whose colors are the most apart, for how many
// pixels have them, is cut in two halves of as many pixels.
static void cut(Quantizer *q) {
  q->boxes[0] = (Box){.from = 0, .to = q->nused};
  q->nboxes = q->nused != 0;

  if (q->nboxes) {
    measure(q, &q->boxes[0]);
  }

  while (q->nboxes != 0 && q->nboxes < COLORS) {
    Box *widest = nullptr;
    uint64_t most = 0;

    for (uint32_t i = 0; i < q->nboxes; i++) {
      uint64_t weight = q->boxes[i].count * q->boxes[i].range;

      if (weight > most) {
        most = weight;
        widest = &q->boxes[i];
      }
    }

    if (widest == nullptr) {
      break;
    }

    order(q, widest);

    uint64_t half = 0;
    uint32_t at = widest->from;

    // colors that differ are two bins at least, so both halves have one.
    do {
      half += q->bins[q->used[at++]].count;
    } while (at + 1 < widest->to && half * 2 < widest->count);

    Box *other = &q->boxes[q->nboxes++];

    *other = (Box){.from = at, .to = widest->to};
    widest->to = at;
    measure(q, widest);
    measure(q, other);
  }
}

// a frame's colors as a palette, and each pixel as its place in it. the
// first of the palette is what is see-through.
static void quantize(Quantizer *q, const uint8_t *rgba, size_t pixels,
                     uint8_t palette[static 768], uint8_t *indexes) {
  q->nused = 0;

  for (size_t i = 0; i < pixels; i++) {
    const uint8_t *p = rgba + i * 4;
    uint32_t a = p[3], c[3];

    // a gif has no half: what is less than half there is not.
    if (a < 128) {
      q->of[i] = CLEAR;
      continue;
    }

    for (uint32_t k = 0; k < 3; k++) {
      c[k] = (p[k] * 255 + a / 2) / a;
      c[k] = c[k] > 255 ? 255 : c[k];
    }

    uint16_t bin = (uint16_t)((c[0] >> 3) << 10 | (c[1] >> 3) << 5 | c[2] >> 3);
    Bin *held = &q->bins[bin];

    if (held->count == 0) {
      q->used[q->nused++] = bin;
    }

    held->count++;
    held->sum[0] += c[0];
    held->sum[1] += c[1];
    held->sum[2] += c[2];
    q->of[i] = bin;
  }

  cut(q);
  memset(palette, 0, 768);

  for (uint32_t i = 0; i < q->nboxes; i++) {
    const Box *box = &q->boxes[i];
    uint64_t sum[3] = {}, count = 0;

    for (uint32_t j = box->from; j < box->to; j++) {
      const Bin *bin = &q->bins[q->used[j]];

      count += bin->count;
      sum[0] += bin->sum[0];
      sum[1] += bin->sum[1];
      sum[2] += bin->sum[2];
      q->lut[q->used[j]] = (uint8_t)(i + 1);
    }

    for (uint32_t k = 0; count != 0 && k < 3; k++) {
      palette[(i + 1) * 3 + k] = (uint8_t)((sum[k] + count / 2) / count);
    }
  }

  for (size_t i = 0; i < pixels; i++) {
    indexes[i] = q->of[i] == CLEAR ? 0 : q->lut[q->of[i]];
  }

  for (uint32_t i = 0; i < q->nused; i++) {
    q->bins[q->used[i]] = (Bin){};
  }
}

// gif's lzw: codes of nine bits and more, least bit first, in blocks of
// 255 bytes at most.
constexpr uint32_t LZW_SLOTS = 1u << 13;
constexpr uint32_t LZW_CLEAR = 256;
constexpr uint32_t LZW_END = 257;
constexpr uint32_t LZW_LAST = 4095;

typedef struct {
  Bytes *out;
  uint8_t block[255];
  uint32_t held, bits, nblock;
  // a code and a byte after it, to the code of both.
  uint32_t keys[LZW_SLOTS];
  uint16_t codes[LZW_SLOTS];
} Lzw;

static void lzw_byte(Lzw *z, uint8_t byte) {
  z->block[z->nblock++] = byte;

  if (z->nblock == sizeof z->block) {
    put8(z->out, (uint8_t)z->nblock);
    put(z->out, z->block, z->nblock);
    z->nblock = 0;
  }
}

static void lzw_code(Lzw *z, uint32_t code, uint32_t width) {
  z->held |= code << z->bits;
  z->bits += width;

  while (z->bits >= 8) {
    lzw_byte(z, (uint8_t)z->held);
    z->held >>= 8;
    z->bits -= 8;
  }
}

static void lzw(Bytes *out, const uint8_t *indexes, size_t n, Lzw *z) {
  uint32_t width = 9, last = LZW_END, code = indexes[0];

  z->out = out;
  z->held = z->bits = z->nblock = 0;
  memset(z->keys, 0xff, sizeof z->keys);
  put8(out, 8);
  lzw_code(z, LZW_CLEAR, width);

  for (size_t i = 1; i < n; i++) {
    uint32_t key = code << 8 | indexes[i];
    uint32_t slot = (key * 2654435761u) >> 19;

    while (z->keys[slot] != UINT32_MAX && z->keys[slot] != key) {
      slot = (slot + 1) & (LZW_SLOTS - 1);
    }

    if (z->keys[slot] == key) {
      code = z->codes[slot];
      continue;
    }

    lzw_code(z, code, width);
    z->keys[slot] = key;
    z->codes[slot] = (uint16_t)++last;
    code = indexes[i];

    if (last >= 1u << width) {
      width++;
    }

    if (last == LZW_LAST) {
      lzw_code(z, LZW_CLEAR, width);
      memset(z->keys, 0xff, sizeof z->keys);
      width = 9;
      last = LZW_END;
    }
  }

  lzw_code(z, code, width);
  lzw_code(z, LZW_END, width);

  if (z->bits) {
    lzw_byte(z, (uint8_t)z->held);
  }

  if (z->nblock) {
    put8(out, (uint8_t)z->nblock);
    put(out, z->block, z->nblock);
  }

  put8(out, 0);
}

uint8_t *fake_sticker_gif(const uint8_t *rgba, const float *delays,
                          uint32_t width, uint32_t height, uint32_t count,
                          size_t *len) {
  size_t pixels = (size_t)width * height;
  Quantizer *q = calloc(1, sizeof *q);
  Lzw *z = malloc(sizeof *z);
  uint8_t *indexes = malloc(pixels != 0 ? pixels : 1);
  Bytes out = {};

  if (q != nullptr) {
    q->bins = calloc(BINS, sizeof *q->bins);
    q->used = malloc(BINS * sizeof *q->used);
    q->sorted = malloc(BINS * sizeof *q->sorted);
    q->of = malloc((pixels != 0 ? pixels : 1) * sizeof *q->of);
    q->lut = malloc(BINS);
  }

  bool ok = pixels != 0 && count != 0 && width <= UINT16_MAX &&
            height <= UINT16_MAX && q != nullptr && z != nullptr &&
            indexes != nullptr && q->bins != nullptr && q->used != nullptr &&
            q->sorted != nullptr && q->of != nullptr && q->lut != nullptr;

  if (ok) {
    put(&out, "GIF89a", 6);
    put16(&out, width);
    put16(&out, height);
    // no palette for all: each frame has its own.
    put(&out, (uint8_t[]){0x70, 0, 0}, 3);
    // forever.
    put(&out, "\x21\xff\x0bNETSCAPE2.0\x03\x01\x00\x00\x00", 19);
  }

  for (uint32_t i = 0; ok && i < count; i++) {
    uint8_t palette[768];
    // hundredths of a second. browsers slow what is shorter than two down.
    uint32_t delay = (uint32_t)(delays[i] * 100 + .5f);

    delay = delay < 2 ? 2 : delay > UINT16_MAX ? UINT16_MAX : delay;
    quantize(q, rgba + (size_t)i * pixels * 4, pixels, palette, indexes);

    // gone before the next, with the first color see-through.
    put(&out, (uint8_t[]){0x21, 0xf9, 4, 2 << 2 | 1}, 4);
    put16(&out, delay);
    put(&out, (uint8_t[]){0, 0, 0x2c}, 3);
    put16(&out, 0);
    put16(&out, 0);
    put16(&out, width);
    put16(&out, height);
    put8(&out, 0x87);
    put(&out, palette, sizeof palette);
    lzw(&out, indexes, pixels, z);
  }

  put8(&out, 0x3b);

  if (q != nullptr) {
    free(q->bins);
    free(q->used);
    free(q->sorted);
    free(q->of);
    free(q->lut);
  }

  free(q);
  free(z);
  free(indexes);

  if (!ok || out.failed) {
    free(out.v);
    return nullptr;
  }

  *len = out.len;
  return out.v;
}
