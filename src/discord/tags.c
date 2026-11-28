#include "discord/tags.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const unsigned char SWORD[] = {
#embed "../../assets/discord/tag-badges/sword.svg"
};
static const unsigned char WATER_DROP[] = {
#embed "../../assets/discord/tag-badges/water_drop.svg"
};
static const unsigned char SKULL[] = {
#embed "../../assets/discord/tag-badges/skull.svg"
};
static const unsigned char TOADSTOOL[] = {
#embed "../../assets/discord/tag-badges/toadstool.svg"
};
static const unsigned char MOON[] = {
#embed "../../assets/discord/tag-badges/moon.svg"
};
static const unsigned char LIGHTNING[] = {
#embed "../../assets/discord/tag-badges/lightning.svg"
};
static const unsigned char LEAF[] = {
#embed "../../assets/discord/tag-badges/leaf.svg"
};
static const unsigned char HEART[] = {
#embed "../../assets/discord/tag-badges/heart.svg"
};
static const unsigned char FIRE[] = {
#embed "../../assets/discord/tag-badges/fire.svg"
};
static const unsigned char COMPASS[] = {
#embed "../../assets/discord/tag-badges/compass.svg"
};
static const unsigned char CROSSHAIRS[] = {
#embed "../../assets/discord/tag-badges/crosshairs.svg"
};
static const unsigned char FLOWER[] = {
#embed "../../assets/discord/tag-badges/flower.svg"
};
static const unsigned char FORCE[] = {
#embed "../../assets/discord/tag-badges/force.svg"
};
static const unsigned char GEM[] = {
#embed "../../assets/discord/tag-badges/gem.svg"
};
static const unsigned char LAVA[] = {
#embed "../../assets/discord/tag-badges/lava.svg"
};
static const unsigned char PSYCHIC[] = {
#embed "../../assets/discord/tag-badges/psychic.svg"
};
static const unsigned char SMOKE[] = {
#embed "../../assets/discord/tag-badges/smoke.svg"
};
static const unsigned char SNOW[] = {
#embed "../../assets/discord/tag-badges/snow.svg"
};
static const unsigned char SOUND[] = {
#embed "../../assets/discord/tag-badges/sound.svg"
};
static const unsigned char SUN[] = {
#embed "../../assets/discord/tag-badges/sun.svg"
};
static const unsigned char WIND[] = {
#embed "../../assets/discord/tag-badges/wind.svg"
};
static const unsigned char BUNNY[] = {
#embed "../../assets/discord/tag-badges/bunny.svg"
};
static const unsigned char DOG[] = {
#embed "../../assets/discord/tag-badges/dog.svg"
};
static const unsigned char FROG[] = {
#embed "../../assets/discord/tag-badges/frog.svg"
};
static const unsigned char GOAT[] = {
#embed "../../assets/discord/tag-badges/goat.svg"
};
static const unsigned char CAT[] = {
#embed "../../assets/discord/tag-badges/cat.svg"
};
static const unsigned char DIAMOND[] = {
#embed "../../assets/discord/tag-badges/diamond.svg"
};
static const unsigned char CROWN[] = {
#embed "../../assets/discord/tag-badges/crown.svg"
};
static const unsigned char TROPHY[] = {
#embed "../../assets/discord/tag-badges/trophy.svg"
};
static const unsigned char MONEY_BAG[] = {
#embed "../../assets/discord/tag-badges/money_bag.svg"
};
static const unsigned char DOLLAR_SIGN[] = {
#embed "../../assets/discord/tag-badges/dollar_sign.svg"
};
static const unsigned char CLOVER[] = {
#embed "../../assets/discord/tag-badges/clover.svg"
};
static const unsigned char BLOSSOM[] = {
#embed "../../assets/discord/tag-badges/blossom.svg"
};
static const unsigned char POTTED_PLANT[] = {
#embed "../../assets/discord/tag-badges/potted_plant.svg"
};
static const unsigned char MAPLE[] = {
#embed "../../assets/discord/tag-badges/maple.svg"
};
static const unsigned char WILTED_FLOWER[] = {
#embed "../../assets/discord/tag-badges/wilted_flower.svg"
};
static const unsigned char BUTTERFLY[] = {
#embed "../../assets/discord/tag-badges/butterfly.svg"
};
static const unsigned char SNAIL[] = {
#embed "../../assets/discord/tag-badges/snail.svg"
};
static const unsigned char CATERPILLAR[] = {
#embed "../../assets/discord/tag-badges/caterpillar.svg"
};
static const unsigned char SPIDER[] = {
#embed "../../assets/discord/tag-badges/spider.svg"
};
static const unsigned char BEE[] = {
#embed "../../assets/discord/tag-badges/bee.svg"
};

const TagBadge TAG_BADGE_ART[TAG_BADGES] = {
  {
    .name = "sword",
    .label = "Sword",
    .svg = SWORD,
    .len = sizeof SWORD,
    .colors = 2,
    .nslots = 6,
    .slots = {
      {true, 0.675, 0.25},
      {false, 0.32, 0.2},
      {true, 0.24, 0.20000000000000007},
      {false, 0.08333333333333333, 0.16666666666666669},
      {true, 0.8888888888888888, 0.11111111111111116},
      {false, 0.5249999999999999, 0.25},
    },
  },
  {
    .name = "water_drop",
    .label = "Water drop",
    .svg = WATER_DROP,
    .len = sizeof WATER_DROP,
    .colors = 1,
    .nslots = 3,
    .slots = {
      {false, 0.24, 0.25},
      {false, 0.07500000000000001, 0.25},
      {false, 0.9090909090909091, 0.09090909090909094},
    },
  },
  {
    .name = "skull",
    .label = "Skull",
    .svg = SKULL,
    .len = sizeof SKULL,
    .colors = 1,
    .nslots = 3,
    .slots = {
      {false, 0.39999999999999997, 0.3333333333333334},
      {false, 0.09999999999999999, 0.16666666666666669},
      {false, 0.0, 0.09090909090909091},
    },
  },
  {
    .name = "toadstool",
    .label = "Toadstool",
    .svg = TOADSTOOL,
    .len = sizeof TOADSTOOL,
    .colors = 2,
    .nslots = 4,
    .slots = {
      {true, 0.64, 0.20000000000000007},
      {false, 0.2, 0.2},
      {false, 0.09999999999999999, 0.16666666666666669},
      {true, 0.3333333333333333, 0.16666666666666669},
    },
  },
  {
    .name = "moon",
    .label = "Moon",
    .svg = MOON,
    .len = sizeof MOON,
    .colors = 1,
    .nslots = 3,
    .slots = {
      {false, 0.3375, 0.25},
      {false, 0.75, 0.16666666666666663},
      {false, 0.125, 0.16666666666666669},
    },
  },
  {
    .name = "lightning",
    .label = "Lightning",
    .svg = LIGHTNING,
    .len = sizeof LIGHTNING,
    .colors = 1,
    .nslots = 3,
    .slots = {
      {false, 0.3, 0.39999999999999997},
      {false, 0.8181818181818182, 0.09090909090909083},
      {false, 0.125, 0.16666666666666669},
    },
  },
  {
    .name = "leaf",
    .label = "Leaf",
    .svg = LEAF,
    .len = sizeof LEAF,
    .colors = 1,
    .nslots = 3,
    .slots = {
      {false, 0.44999999999999996, 0.25},
      {false, 0.15000000000000002, 0.25},
      {false, 0.08, 0.19999999999999996},
    },
  },
  {
    .name = "heart",
    .label = "Heart",
    .svg = HEART,
    .len = sizeof HEART,
    .colors = 1,
    .nslots = 3,
    .slots = {
      {false, 0.3466666666666667, 0.33333333333333337},
      {false, 0.15000000000000002, 0.25},
      {false, 0.8333333333333334, 0.16666666666666663},
    },
  },
  {
    .name = "fire",
    .label = "Fire",
    .svg = FIRE,
    .len = sizeof FIRE,
    .colors = 1,
    .nslots = 3,
    .slots = {
      {false, 0.22499999999999998, 0.25},
      {false, 0.07500000000000001, 0.25},
      {false, 0.9090909090909091, 0.09090909090909094},
    },
  },
  {
    .name = "compass",
    .label = "Compass",
    .svg = COMPASS,
    .len = sizeof COMPASS,
    .colors = 2,
    .nslots = 5,
    .slots = {
      {true, 0.16666666666666666, 0.33333333333333337},
      {false, 0.16666666666666666, 0.33333333333333337},
      {true, 0.6666666666666666, 0.33333333333333337},
      {true, 0.07500000000000001, 0.25},
      {false, 0.07500000000000001, 0.25},
    },
  },
  {
    .name = "crosshairs",
    .label = "Crosshairs",
    .svg = CROSSHAIRS,
    .len = sizeof CROSSHAIRS,
    .colors = 2,
    .nslots = 7,
    .slots = {
      {false, 0.4666666666666666, 0.3333333333333333},
      {false, 0.8, 0.19999999999999996},
      {false, 0.27999999999999997, 0.2},
      {true, 0.39999999999999997, 0.3333333333333334},
      {true, 0.8, 0.19999999999999996},
      {true, 0.11249999999999999, 0.25},
      {false, 0.044444444444444446, 0.1111111111111111},
    },
  },
  {
    .name = "flower",
    .label = "Flower",
    .svg = FLOWER,
    .len = sizeof FLOWER,
    .colors = 2,
    .nslots = 6,
    .slots = {
      {false, 0.36, 0.19999999999999996},
      {true, 0.27999999999999997, 0.2},
      {true, 0.08888888888888889, 0.11111111111111112},
      {false, 0.08888888888888889, 0.11111111111111112},
      {true, 0.8571428571428571, 0.1428571428571429},
      {false, 0.8571428571428571, 0.1428571428571429},
    },
  },
  {
    .name = "force",
    .label = "Force",
    .svg = FORCE,
    .len = sizeof FORCE,
    .colors = 2,
    .nslots = 6,
    .slots = {
      {true, 0.6666666666666666, 0.33333333333333337},
      {false, 0.6666666666666666, 0.33333333333333337},
      {true, 0.375, 0.25},
      {true, 0.26666666666666666, 0.1111111111111111},
      {false, 0.30000000000000004, 0.25},
      {false, 0.26666666666666666, 0.1111111111111111},
    },
  },
  {
    .name = "gem",
    .label = "Gem",
    .svg = GEM,
    .len = sizeof GEM,
    .colors = 2,
    .nslots = 8,
    .slots = {
      {true, 0.24, 0.20000000000000007},
      {false, 0.24, 0.20000000000000007},
      {false, 0.72, 0.19999999999999996},
      {false, 0.375, 0.25},
      {false, 0.08888888888888889, 0.11111111111111112},
      {true, 0.72, 0.19999999999999996},
      {true, 0.375, 0.25},
      {true, 0.08888888888888889, 0.11111111111111112},
    },
  },
  {
    .name = "lava",
    .label = "Lava",
    .svg = LAVA,
    .len = sizeof LAVA,
    .colors = 2,
    .nslots = 7,
    .slots = {
      {false, 0.30000000000000004, 0.25},
      {true, 0.24, 0.20000000000000007},
      {true, 0.03333333333333333, 0.33333333333333337},
      {false, 0.044444444444444446, 0.1111111111111111},
      {false, 0.8, 0.11111111111111094},
      {true, 0.76, 0.19999999999999996},
      {false, 0.125, 0.16666666666666669},
    },
  },
  {
    .name = "psychic",
    .label = "Psychic",
    .svg = PSYCHIC,
    .len = sizeof PSYCHIC,
    .colors = 2,
    .nslots = 5,
    .slots = {
      {false, 0.48, 0.19999999999999996},
      {false, 0.8444444444444444, 0.11111111111111105},
      {false, 0.08888888888888889, 0.11111111111111112},
      {true, 0.08888888888888889, 0.11111111111111112},
      {true, 0.2571428571428571, 0.14285714285714285},
    },
  },
  {
    .name = "smoke",
    .label = "Smoke",
    .svg = SMOKE,
    .len = sizeof SMOKE,
    .colors = 2,
    .nslots = 6,
    .slots = {
      {true, 0.3333333333333333, 0.3333333333333333},
      {false, 0.13333333333333333, 0.33333333333333326},
      {false, 0.04, 0.19999999999999998},
      {true, 0.16, 0.19999999999999998},
      {true, 0.7124999999999999, 0.25},
      {false, 0.7124999999999999, 0.25},
    },
  },
  {
    .name = "snow",
    .label = "Snow",
    .svg = SNOW,
    .len = sizeof SNOW,
    .colors = 2,
    .nslots = 6,
    .slots = {
      {false, 0.29999999999999993, 0.14285714285714285},
      {false, 0.08888888888888889, 0.11111111111111112},
      {true, 0.3, 0.5},
      {true, 0.08, 0.19999999999999996},
      {true, 0.8888888888888888, 0.11111111111111116},
      {false, 0.8888888888888888, 0.11111111111111116},
    },
  },
  {
    .name = "sound",
    .label = "Sound",
    .svg = SOUND,
    .len = sizeof SOUND,
    .colors = 2,
    .nslots = 7,
    .slots = {
      {false, 0.26666666666666666, 0.3333333333333333},
      {false, 0.7111111111111111, 0.11111111111111116},
      {true, 0.8888888888888888, 0.11111111111111116},
      {false, 0.8888888888888888, 0.11111111111111116},
      {true, 0.25, 0.5},
      {true, 0.08, 0.19999999999999996},
      {false, 0.08888888888888889, 0.11111111111111112},
    },
  },
  {
    .name = "sun",
    .label = "Sun",
    .svg = SUN,
    .len = sizeof SUN,
    .colors = 2,
    .nslots = 6,
    .slots = {
      {false, 0.3375, 0.25},
      {false, 0.8888888888888888, 0.11111111111111116},
      {true, 0.8, 0.19999999999999996},
      {true, 0.06666666666666667, 0.3333333333333333},
      {false, 0.05600000000000001, 0.2},
      {true, 0.13333333333333333, 0.6666666666666666},
    },
  },
  {
    .name = "wind",
    .label = "Wind",
    .svg = WIND,
    .len = sizeof WIND,
    .colors = 2,
    .nslots = 6,
    .slots = {
      {false, 0.175, 0.5},
      {true, 0.375, 0.5},
      {true, 0.13333333333333333, 0.33333333333333326},
      {true, 0.8571428571428571, 0.1428571428571429},
      {false, 0.05600000000000001, 0.2},
      {false, 0.8888888888888888, 0.11111111111111116},
    },
  },
  {
    .name = "bunny",
    .label = "Bunny",
    .svg = BUNNY,
    .len = sizeof BUNNY,
    .colors = 1,
    .nslots = 2,
    .slots = {
      {false, 0.48750000000000004, 0.25},
      {false, 0.16, 0.19999999999999998},
    },
  },
  {
    .name = "dog",
    .label = "Dog",
    .svg = DOG,
    .len = sizeof DOG,
    .colors = 2,
    .nslots = 3,
    .slots = {
      {false, 0.30000000000000004, 0.25},
      {true, 0.6857142857142858, 0.1428571428571428},
      {false, 0.041666666666666664, 0.16666666666666669},
    },
  },
  {
    .name = "frog",
    .label = "Frog",
    .svg = FROG,
    .len = sizeof FROG,
    .colors = 2,
    .nslots = 4,
    .slots = {
      {false, 0.375, 0.25},
      {true, 0.6000000000000001, 0.25},
      {true, 0.3333333333333333, 0.16666666666666669},
      {false, 0.16666666666666666, 0.16666666666666666},
    },
  },
  {
    .name = "goat",
    .label = "Goat",
    .svg = GOAT,
    .len = sizeof GOAT,
    .colors = 1,
    .nslots = 2,
    .slots = {
      {false, 0.375, 0.25},
      {false, 0.17500000000000002, 0.12500000000000003},
    },
  },
  {
    .name = "cat",
    .label = "Cat",
    .svg = CAT,
    .len = sizeof CAT,
    .colors = 1,
    .nslots = 3,
    .slots = {
      {false, 0.32, 0.2},
      {false, 0.5999999999999999, 0.1428571428571429},
      {false, 0.09090909090909091, 0.09090909090909091},
    },
  },
  {
    .name = "diamond",
    .label = "Diamond",
    .svg = DIAMOND,
    .len = sizeof DIAMOND,
    .colors = 1,
    .nslots = 2,
    .slots = {
      {false, 0.315, 0.24999999999999994},
      {false, 0.13125, 0.12499999999999997},
    },
  },
  {
    .name = "crown",
    .label = "Crown",
    .svg = CROWN,
    .len = sizeof CROWN,
    .colors = 2,
    .nslots = 4,
    .slots = {
      {false, 0.5775, 0.25},
      {false, 0.42000000000000004, 0.25},
      {true, 0.17500000000000002, 0.12500000000000003},
      {true, 0.30000000000000004, 0.25},
    },
  },
  {
    .name = "trophy",
    .label = "Trophy",
    .svg = TROPHY,
    .len = sizeof TROPHY,
    .colors = 1,
    .nslots = 3,
    .slots = {
      {false, 0.5775, 0.25},
      {false, 0.42000000000000004, 0.25},
      {false, 0.25, 0.16666666666666669},
    },
  },
  {
    .name = "money_bag",
    .label = "Money bag",
    .svg = MONEY_BAG,
    .len = sizeof MONEY_BAG,
    .colors = 1,
    .nslots = 2,
    .slots = {
      {false, 0.5775, 0.25},
      {false, 0.4166666666666667, 0.16666666666666669},
    },
  },
  {
    .name = "dollar_sign",
    .label = "Dollar sign",
    .svg = DOLLAR_SIGN,
    .len = sizeof DOLLAR_SIGN,
    .colors = 1,
    .nslots = 2,
    .slots = {
      {false, 0.44999999999999996, 0.25},
      {false, 0.1416666666666667, 0.16666666666666666},
    },
  },
  {
    .name = "clover",
    .label = "Clover",
    .svg = CLOVER,
    .len = sizeof CLOVER,
    .colors = 1,
    .nslots = 3,
    .slots = {
      {false, 0.4275, 0.25},
      {false, 0.3225, 0.25},
      {false, 0.16666666666666666, 0.16666666666666666},
    },
  },
  {
    .name = "blossom",
    .label = "Blossom",
    .svg = BLOSSOM,
    .len = sizeof BLOSSOM,
    .colors = 1,
    .nslots = 4,
    .slots = {
      {false, 0.20833333333333334, 0.16666666666666666},
      {false, 0.44999999999999996, 0.25},
      {false, 0.5249999999999999, 0.25},
      {false, 0.375, 0.25},
    },
  },
  {
    .name = "potted_plant",
    .label = "Potted plant",
    .svg = POTTED_PLANT,
    .len = sizeof POTTED_PLANT,
    .colors = 2,
    .nslots = 6,
    .slots = {
      {false, 0.08333333333333333, 0.16666666666666669},
      {false, 0.3375, 0.25},
      {false, 0.1875, 0.25},
      {true, 0.375, 0.16666666666666663},
      {true, 0.5625, 0.25},
      {true, 0.44999999999999996, 0.25},
    },
  },
  {
    .name = "maple",
    .label = "Maple leaf",
    .svg = MAPLE,
    .len = sizeof MAPLE,
    .colors = 1,
    .nslots = 5,
    .slots = {
      {false, 0.1875, 0.25},
      {false, 0.48750000000000004, 0.25},
      {false, 0.1416666666666667, 0.16666666666666666},
      {false, 0.30000000000000004, 0.25},
      {false, 0.3975, 0.24999999999999994},
    },
  },
  {
    .name = "wilted_flower",
    .label = "Wilted flower",
    .svg = WILTED_FLOWER,
    .len = sizeof WILTED_FLOWER,
    .colors = 2,
    .nslots = 5,
    .slots = {
      {true, 0.41250000000000003, 0.25000000000000006},
      {false, 0.08333333333333333, 0.16666666666666669},
      {false, 0.22499999999999998, 0.25},
      {false, 0.41250000000000003, 0.25000000000000006},
      {true, 0.20833333333333334, 0.16666666666666666},
    },
  },
  {
    .name = "butterfly",
    .label = "Butterfly",
    .svg = BUTTERFLY,
    .len = sizeof BUTTERFLY,
    .colors = 2,
    .nslots = 3,
    .slots = {
      {false, 0.3375, 0.25},
      {false, 0.5249999999999999, 0.25},
      {true, 0.15000000000000002, 0.25},
    },
  },
  {
    .name = "snail",
    .label = "Snail",
    .svg = SNAIL,
    .len = sizeof SNAIL,
    .colors = 2,
    .nslots = 4,
    .slots = {
      {false, 0.3375, 0.25},
      {false, 0.20833333333333334, 0.16666666666666666},
      {true, 0.5625, 0.25},
      {true, 0.4166666666666667, 0.16666666666666669},
    },
  },
  {
    .name = "caterpillar",
    .label = "Caterpillar",
    .svg = CATERPILLAR,
    .len = sizeof CATERPILLAR,
    .colors = 2,
    .nslots = 3,
    .slots = {
      {false, 0.3375, 0.25},
      {false, 0.20833333333333334, 0.16666666666666666},
      {true, 0.5249999999999999, 0.25},
    },
  },
  {
    .name = "spider",
    .label = "Spider",
    .svg = SPIDER,
    .len = sizeof SPIDER,
    .colors = 2,
    .nslots = 3,
    .slots = {
      {false, 0.3375, 0.25},
      {true, 0.125, 0.16666666666666669},
      {false, 0.16666666666666666, 0.16666666666666666},
    },
  },
  {
    .name = "bee",
    .label = "Bee",
    .svg = BEE,
    .len = sizeof BEE,
    .colors = 1,
    .nslots = 2,
    .slots = {
      {false, 0.5625, 0.25},
      {false, 0.4166666666666667, 0.16666666666666669},
    },
  },
};

const TagPalette TAG_PALETTE[TAG_PALETTES] = {
  {0xff1c90, 0xff7fc0}, {0xff8f1c, 0xffae77}, {0xeebe1a, 0xfffc7f},
  {0x32a070, 0x57b59e}, {0x32839a, 0x71c2d9}, {0x8a43ff, 0xbd95ff},
  {0x9b3fe5, 0xcc8dff}, {0x942e8f, 0xd46cb5}, {0xd14242, 0xff8989},
  {0x814300, 0xa88a6c}, {0x717224, 0xc3c3c3}, {0x5d1d47, 0xc58fbb},
  {0x222222, 0xcccccc},
};

const uint8_t TAG_FREE_BADGE[TAG_FREE_BADGES] = {6, 0, 7, 8, 1, 2, 4, 5, 9, 3};

const TagPack TAG_PACK[TAG_PACKS] = {
  {"GUILD_TAGS_BADGE_PACK_PETS", 21},
  {"GUILD_TAGS_BADGE_PACK_FLEX", 26},
  {"GUILD_TAGS_BADGE_PACK_PLANT", 31},
  {"GUILD_TAGS_BADGE_PACK_CREEPY_CRAWLIES", 36},
};

/* ========================================================================== */
/* chroma-js's luminance, which discord's client tints badges with.           */
/* ========================================================================== */

static double channel(double c) {
  c /= 255;
  return c <= 0.03928 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

static double luminance(const double rgb[3]) {
  return 0.2126 * channel(rgb[0]) + 0.7152 * channel(rgb[1]) +
         0.0722 * channel(rgb[2]);
}

// halves the way between `low` and `high` until it meets `target`, twenty
// one halvings at most as chroma-js stops.
static void seek(const double low[3], const double high[3], double target,
                 int left, double out[3]) {
  double mid[3];

  for (int i = 0; i < 3; i++) {
    mid[i] = low[i] + (high[i] - low[i]) / 2;
  }

  double at = luminance(mid);

  if (fabs(target - at) < 1e-7 || left == 0) {
    memcpy(out, mid, sizeof mid);
  }
  else if (at > target) {
    seek(low, mid, target, left - 1, out);
  }
  else {
    seek(mid, high, target, left - 1, out);
  }
}

uint32_t tag_tint(const TagSlot *slot, uint32_t tint) {
  double rgb[3] = {tint >> 16 & 0xff, tint >> 8 & 0xff, tint & 0xff};
  double target = slot->x0 + slot->w * luminance(rgb);
  double out[3];

  if (target == 0) {
    return 0x000000;
  }

  if (target == 1) {
    return 0xffffff;
  }

  if (luminance(rgb) > target) {
    seek((double[3]){0, 0, 0}, rgb, target, 20, out);
  }
  else {
    seek(rgb, (double[3]){255, 255, 255}, target, 20, out);
  }

  uint32_t color = 0;

  for (int i = 0; i < 3; i++) {
    color = color << 8 | (uint32_t)floor(out[i] + 0.5);
  }

  return color;
}

/* ========================================================================== */
/* the svgs.                                                                  */
/* ========================================================================== */

void tag_badge_url(char out[static TAG_BADGE_URL_MAX], uint8_t badge,
                   int32_t primary, int32_t secondary) {
  snprintf(out, TAG_BADGE_URL_MAX, "tag-badge:%u:%d:%d", badge, primary,
           secondary);
}

const char *tag_badge_svg(const char *url, size_t *len) {
  static char *svg;
  unsigned badge = 0;
  int32_t tints[2];
  int read = 0;

  if (strncmp(url, "tag-badge:", 10) != 0 ||
      sscanf(url + 10, "%u:%d:%d%n", &badge, &tints[0], &tints[1], &read) !=
        3 ||
      url[10 + read] != '\0' || badge >= TAG_BADGES) {
    return nullptr;
  }

  const TagBadge *art = &TAG_BADGE_ART[badge];

  free(svg);
  svg = malloc(art->len + 1);

  if (svg == nullptr) {
    return nullptr;
  }

  memcpy(svg, art->svg, art->len);
  svg[art->len] = '\0';

  // each tinted color follows its mark: data-tint="n" fill="#rrggbb".
  for (char *at = svg; (at = strstr(at, "data-tint=\"")) != nullptr;) {
    unsigned slot = (unsigned)strtoul(at + 11, &at, 10);
    char *color = strstr(at, "=\"#");

    if (color == nullptr || slot >= art->nslots) {
      break;
    }

    int32_t tint = tints[art->slots[slot].secondary];

    if (tint != TAG_TINT_NONE) {
      char hex[8];

      snprintf(hex, sizeof hex, "%06x",
               tag_tint(&art->slots[slot], (uint32_t)tint) & 0xffffff);
      memcpy(color + 3, hex, 6);
    }

    at = color + 9;
  }

  *len = art->len;
  return svg;
}
