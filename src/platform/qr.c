#include <stddefer.h>
#include <stdlib.h>
#include <string.h>

#include "platform/platform.h"

// a qr code of the medium level of error correction holding bytes, as the
// standard has it, iso 18004.

constexpr int VERSIONS = 40;
constexpr int ECC_MOST = 30;
constexpr int BLOCKS_MOST = 49;
constexpr int BLOCK_MOST = 154;
constexpr int CODEWORDS_MOST = 3706;

// for the medium level, by version: the error correction codewords of each
// block, and how many blocks.
static const uint8_t ECC_PER_BLOCK[VERSIONS + 1] = {
  0,  10, 16, 26, 18, 24, 16, 18, 22, 22, 26, 30, 22, 22,
  24, 24, 28, 28, 26, 26, 26, 26, 28, 28, 28, 28, 28, 28,
  28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28,
};
static const uint8_t BLOCKS[VERSIONS + 1] = {
  0,  1,  1,  1,  2,  2,  4,  4,  4,  5,  5,  5,  8,  9,
  9,  10, 10, 11, 13, 14, 16, 17, 17, 18, 20, 21, 23, 25,
  26, 28, 29, 31, 33, 35, 37, 38, 40, 43, 45, 47, 49,
};

typedef struct {
  int side;
  uint8_t *dark;
  // the finders, the timing, the alignment and the format and version
  // bits, which the data goes around and the mask leaves alone.
  uint8_t *fixed;
} Code;

// the modules data can go in, every one that is not fixed.
static int raw_modules(int version) {
  int n = (16 * version + 128) * version + 64;

  if (version >= 2) {
    int aligns = version / 7 + 2;

    n -= (25 * aligns - 10) * aligns - 55;
    n -= version >= 7 ? 36 : 0;
  }

  return n;
}

static int data_codewords(int version) {
  return raw_modules(version) / 8 - ECC_PER_BLOCK[version] * BLOCKS[version];
}

static void put(Code *c, int x, int y, bool dark) {
  c->dark[y * c->side + x] = dark;
  c->fixed[y * c->side + x] = 1;
}

static void finder(Code *c, int cx, int cy) {
  for (int dy = -4; dy <= 4; dy++) {
    for (int dx = -4; dx <= 4; dx++) {
      int x = cx + dx, y = cy + dy;
      int ring = abs(dx) > abs(dy) ? abs(dx) : abs(dy);

      if (x >= 0 && x < c->side && y >= 0 && y < c->side) {
        put(c, x, y, ring != 2 && ring != 4);
      }
    }
  }
}

static void alignment(Code *c, int cx, int cy) {
  for (int dy = -2; dy <= 2; dy++) {
    for (int dx = -2; dx <= 2; dx++) {
      put(c, cx + dx, cy + dy, (abs(dx) > abs(dy) ? abs(dx) : abs(dy)) != 1);
    }
  }
}

// where the alignment patterns are centered on each axis.
static int align_positions(int version, int side, int out[7]) {
  if (version == 1) {
    return 0;
  }

  int n = version / 7 + 2;
  int step = version == 32 ? 26 : (version * 4 + n * 2 + 1) / (n * 2 - 2) * 2;

  out[0] = 6;

  for (int i = n - 1, at = side - 7; i >= 1; i--, at -= step) {
    out[i] = at;
  }

  return n;
}

static void format_bits(Code *c, int mask) {
  // the medium level is 0 in the two bits that name it.
  int data = mask, rem = data;

  for (int i = 0; i < 10; i++) {
    rem = (rem << 1) ^ ((rem >> 9) * 0x537);
  }

  int bits = (data << 10 | rem) ^ 0x5412;
  int side = c->side;

  for (int i = 0; i <= 5; i++) {
    put(c, 8, i, bits >> i & 1);
  }

  put(c, 8, 7, bits >> 6 & 1);
  put(c, 8, 8, bits >> 7 & 1);
  put(c, 7, 8, bits >> 8 & 1);

  for (int i = 9; i < 15; i++) {
    put(c, 14 - i, 8, bits >> i & 1);
  }

  for (int i = 0; i < 8; i++) {
    put(c, side - 1 - i, 8, bits >> i & 1);
  }

  for (int i = 8; i < 15; i++) {
    put(c, 8, side - 15 + i, bits >> i & 1);
  }

  put(c, 8, side - 8, true);
}

static void version_bits(Code *c, int version) {
  if (version < 7) {
    return;
  }

  int rem = version;

  for (int i = 0; i < 12; i++) {
    rem = (rem << 1) ^ ((rem >> 11) * 0x1f25);
  }

  int bits = version << 12 | rem;

  for (int i = 0; i < 18; i++) {
    int a = c->side - 11 + i % 3, b = i / 3;

    put(c, a, b, bits >> i & 1);
    put(c, b, a, bits >> i & 1);
  }
}

static void fixed_patterns(Code *c, int version) {
  int side = c->side, at[7];

  for (int i = 0; i < side; i++) {
    put(c, 6, i, i % 2 == 0);
    put(c, i, 6, i % 2 == 0);
  }

  finder(c, 3, 3);
  finder(c, side - 4, 3);
  finder(c, 3, side - 4);

  int n = align_positions(version, side, at);

  for (int i = 0; i < n; i++) {
    for (int j = 0; j < n; j++) {
      // the three corners the finders hold.
      if (!((i == 0 && j == 0) || (i == 0 && j == n - 1) ||
            (i == n - 1 && j == 0))) {
        alignment(c, at[i], at[j]);
      }
    }
  }

  format_bits(c, 0);
  version_bits(c, version);
}

static uint8_t gf_mul(uint8_t x, uint8_t y) {
  int z = 0;

  for (int i = 7; i >= 0; i--) {
    z = (z << 1) ^ ((z >> 7) * 0x11d);
    z ^= (y >> i & 1) * x;
  }

  return (uint8_t)z;
}

// the reed solomon codewords of `data` against the generator of `degree`.
static void ecc_of(const uint8_t *data, int len, int degree, uint8_t *out) {
  uint8_t divisor[ECC_MOST] = {};
  uint8_t root = 1;

  divisor[degree - 1] = 1;

  for (int i = 0; i < degree; i++) {
    for (int j = 0; j < degree; j++) {
      divisor[j] = gf_mul(divisor[j], root);

      if (j + 1 < degree) {
        divisor[j] ^= divisor[j + 1];
      }
    }

    root = gf_mul(root, 2);
  }

  memset(out, 0, (size_t)degree);

  for (int i = 0; i < len; i++) {
    uint8_t factor = data[i] ^ out[0];

    memmove(out, out + 1, (size_t)degree - 1);
    out[degree - 1] = 0;

    for (int j = 0; j < degree; j++) {
      out[j] ^= gf_mul(divisor[j], factor);
    }
  }
}

// the data codewords split in blocks, each followed by its error
// correction, then interleaved: the first byte of every block, the second,
// and so on. the short blocks come first and lack the last data byte.
static int interleave(int version, const uint8_t *data, uint8_t *out) {
  int blocks = BLOCKS[version], ecc = ECC_PER_BLOCK[version];
  int raw = raw_modules(version) / 8;
  int shorts = blocks - raw % blocks, short_len = raw / blocks;
  uint8_t all[BLOCKS_MOST][BLOCK_MOST];

  for (int b = 0, from = 0; b < blocks; b++) {
    int len = short_len - ecc + (b < shorts ? 0 : 1);

    memcpy(all[b], data + from, (size_t)len);
    ecc_of(data + from, len, ecc, all[b] + short_len + 1 - ecc);
    from += len;
  }

  int n = 0;

  for (int i = 0; i <= short_len; i++) {
    for (int b = 0; b < blocks; b++) {
      if (i != short_len - ecc || b >= shorts) {
        out[n++] = all[b][i];
      }
    }
  }

  return n;
}

static void place(Code *c, const uint8_t *codewords, int count) {
  int side = c->side, i = 0;

  for (int right = side - 1; right >= 1; right -= 2) {
    right = right == 6 ? 5 : right;

    for (int vert = 0; vert < side; vert++) {
      for (int j = 0; j < 2; j++) {
        int x = right - j;
        bool upward = ((right + 1) & 2) == 0;
        int y = upward ? side - 1 - vert : vert;

        if (!c->fixed[y * side + x] && i < count * 8) {
          c->dark[y * side + x] = codewords[i >> 3] >> (7 - (i & 7)) & 1;
          i++;
        }
      }
    }
  }
}

static bool masked(int mask, int x, int y) {
  switch (mask) {
    case 0:
      return (x + y) % 2 == 0;
    case 1:
      return y % 2 == 0;
    case 2:
      return x % 3 == 0;
    case 3:
      return (x + y) % 3 == 0;
    case 4:
      return (x / 3 + y / 2) % 2 == 0;
    case 5:
      return x * y % 2 + x * y % 3 == 0;
    case 6:
      return (x * y % 2 + x * y % 3) % 2 == 0;
    default:
      return ((x + y) % 2 + x * y % 3) % 2 == 0;
  }
}

static void apply_mask(Code *c, int mask) {
  for (int y = 0; y < c->side; y++) {
    for (int x = 0; x < c->side; x++) {
      if (!c->fixed[y * c->side + x] && masked(mask, x, y)) {
        c->dark[y * c->side + x] ^= 1;
      }
    }
  }
}

static bool dark_at(const Code *c, int x, int y, bool across) {
  return across ? c->dark[y * c->side + x] : c->dark[x * c->side + y];
}

// how hard the code is to read, the lowest of the eight masks is kept.
static int penalty(const Code *c) {
  static const uint8_t FINDER[11] = {1, 0, 1, 1, 1, 0, 1, 0, 0, 0, 0};
  int side = c->side, score = 0, dark = 0;

  for (int across = 0; across < 2; across++) {
    for (int a = 0; a < side; a++) {
      int run = 1;

      for (int b = 1; b <= side; b++) {
        if (b < side &&
            dark_at(c, b, a, across) == dark_at(c, b - 1, a, across)) {
          run++;
          continue;
        }

        score += run >= 5 ? run - 2 : 0;
        run = 1;
      }

      // the finder's look, light around it outside the code too.
      for (int b = -4; b < side; b++) {
        bool ahead = true, behind = true;

        for (int k = 0; k < 11; k++) {
          int at = b + k, back = b + 10 - k;
          bool d = at >= 0 && at < side && dark_at(c, at, a, across);
          bool e = back >= 0 && back < side && dark_at(c, back, a, across);

          ahead = ahead && d == FINDER[k];
          behind = behind && e == FINDER[k];
        }

        score += ahead ? 40 : 0;
        score += behind ? 40 : 0;
      }
    }
  }

  for (int y = 0; y < side; y++) {
    for (int x = 0; x < side; x++) {
      bool d = c->dark[y * side + x];

      dark += d;

      if (x + 1 < side && y + 1 < side && d == c->dark[y * side + x + 1] &&
          d == c->dark[(y + 1) * side + x] &&
          d == c->dark[(y + 1) * side + x + 1]) {
        score += 3;
      }
    }
  }

  int total = side * side;
  int k = (abs(dark * 20 - total * 10) + total - 1) / total - 1;

  return score + (k > 0 ? k : 0) * 10;
}

static void push_bits(uint8_t *data, int *at, int value, int bits) {
  for (int i = bits - 1; i >= 0; i--, (*at)++) {
    data[*at >> 3] |= (uint8_t)((value >> i & 1) << (7 - (*at & 7)));
  }
}

uint8_t *platform_qr(const char *text, size_t *side) {
  int len = (int)strlen(text), version = 1;

  // the mode, the length and the bytes.
  while (version <= VERSIONS &&
         4 + (version < 10 ? 8 : 16) + len * 8 > data_codewords(version) * 8) {
    version++;
  }

  if (version > VERSIONS) {
    return nullptr;
  }

  int capacity = data_codewords(version), at = 0;
  uint8_t data[CODEWORDS_MOST] = {}, codewords[CODEWORDS_MOST];

  push_bits(data, &at, 0b0100, 4);
  push_bits(data, &at, len, version < 10 ? 8 : 16);

  for (int i = 0; i < len; i++) {
    push_bits(data, &at, (uint8_t)text[i], 8);
  }

  // the terminator, up to the next byte, then the pad bytes in turn.
  at += capacity * 8 - at < 4 ? capacity * 8 - at : 4;
  at = (at + 7) / 8 * 8;

  for (uint8_t pad = 0xec; at < capacity * 8; pad ^= 0xec ^ 0x11) {
    push_bits(data, &at, pad, 8);
  }

  int count = interleave(version, data, codewords);
  Code c = {.side = version * 4 + 17};
  size_t area = (size_t)c.side * (size_t)c.side;

  c.dark = calloc(area, 1);
  c.fixed = calloc(area, 1);

  defer free(c.fixed);

  if (c.dark == nullptr || c.fixed == nullptr) {
    free(c.dark);
    return nullptr;
  }

  fixed_patterns(&c, version);
  place(&c, codewords, count);

  int best = 0, lowest = -1;

  for (int mask = 0; mask < 8; mask++) {
    apply_mask(&c, mask);
    format_bits(&c, mask);

    int score = penalty(&c);

    if (lowest < 0 || score < lowest) {
      best = mask;
      lowest = score;
    }

    apply_mask(&c, mask);
  }

  apply_mask(&c, best);
  format_bits(&c, best);
  *side = (size_t)c.side;
  return c.dark;
}
