#include "discord/templates.h"

#include <string.h>

// the channels, roles and codes of discord's client, ids repeated in two
// of them as it repeats them.
const HubTemplate hub_templates[] = {
  {
    .label = "Create New Server",
    .body =
      "{\"icon\":null,\"channels\":[],\"system_channel_id\":null,\"guild_te"
      "mplate_code\":\"2TffvPucqHkN\"}",
  },
  {
    .label = "Study Group",
    .body =
      "{\"icon\":null,\"channels\":[{\"id\":\"00\",\"parent_id\":null,\"nam"
      "e\":\"Info\",\"type\":4},{\"id\":\"01\",\"parent_id\":\"00\",\"name"
      "\":\"notes-resources\",\"type\":0},{\"id\":\"02\",\"parent_id\":\"00"
      "\",\"name\":\"help-and-questions\",\"type\":0},{\"id\":\"10\",\"paren"
      "t_id\":null,\"name\":\"Chat\",\"type\":4},{\"id\":\"11\",\"parent_id"
      "\":\"10\",\"name\":\"introductions\",\"type\":0},{\"id\":\"12\",\"pa"
      "rent_id\":\"10\",\"name\":\"general\",\"type\":0},{\"id\":\"12\",\"p"
      "arent_id\":\"10\",\"name\":\"off-topic\",\"type\":0},{\"id\":\"20\","
      "\"parent_id\":null,\"name\":\"Voice Channels\",\"type\":4},{\"id\":"
      "\"21\",\"parent_id\":\"20\",\"name\":\"Lounge\",\"type\":2},{\"id\":"
      "\"22\",\"parent_id\":\"20\",\"name\":\"Study Room 1\",\"type\":2},{\""
      "id\":\"23\",\"parent_id\":\"20\",\"name\":\"Study Room 2\",\"type\":"
      "2}],\"system_channel_id\":\"12\",\"guild_template_code\":\"2JBhzzca2"
      "vfT\"}",
  },
  {
    .label = "School Club",
    .body =
      "{\"icon\":null,\"channels\":[{\"id\":\"00\",\"parent_id\":null,\"nam"
      "e\":\"Information\",\"type\":4},{\"id\":\"01\",\"parent_id\":\"00\","
      "\"name\":\"welcome-and-rules\",\"type\":0},{\"id\":\"02\",\"parent_i"
      "d\":\"00\",\"name\":\"announcements\",\"type\":0},{\"id\":\"03\",\"p"
      "arent_id\":\"00\",\"name\":\"resources\",\"type\":0},{\"id\":\"10\","
      "\"parent_id\":null,\"name\":\"Text Channels\",\"type\":4},{\"id\":\""
      "11\",\"parent_id\":\"10\",\"name\":\"introductions\",\"type\":0},{\""
      "id\":\"12\",\"parent_id\":\"10\",\"name\":\"general\",\"type\":0},{"
      "\"id\":\"13\",\"parent_id\":\"10\",\"name\":\"off-topic\",\"type\":0}"
      ",{\"id\":\"20\",\"parent_id\":null,\"name\":\"Voice Channels\",\"typ"
      "e\":4},{\"id\":\"21\",\"parent_id\":\"20\",\"name\":\"Lounge\",\"typ"
      "e\":2},{\"id\":\"22\",\"parent_id\":\"20\",\"name\":\"Meeting Room 1"
      "\",\"type\":2},{\"id\":\"22\",\"parent_id\":\"20\",\"name\":\"Meetin"
      "g Room 2\",\"type\":2}],\"system_channel_id\":\"12\",\"roles\":[{\"i"
      "d\":\"00\",\"name\":\"@everyone\",\"permissions\":\"1720707884502593"
      "\"},{\"id\":\"01\",\"name\":\"officers (example)\",\"mentionable\":t"
      "rue,\"hoist\":true,\"permissions\":\"1720709360909911\",\"color\":30"
      "66993}],\"guild_template_code\":\"UMUbvRpRZhS6\"}",
  },
  {
    .label = "Class",
    .body =
      "{\"icon\":null,\"channels\":[{\"id\":\"00\",\"parent_id\":null,\"nam"
      "e\":\"Info\",\"type\":4},{\"id\":\"01\",\"parent_id\":\"00\",\"name"
      "\":\"assignments\",\"type\":0},{\"id\":\"02\",\"parent_id\":\"00\",\""
      "name\":\"help-and-questions\",\"type\":0},{\"id\":\"10\",\"parent_id"
      "\":null,\"name\":\"Chat\",\"type\":4},{\"id\":\"11\",\"parent_id\":"
      "\"10\",\"name\":\"introductions\",\"type\":0},{\"id\":\"12\",\"parent"
      "_id\":\"10\",\"name\":\"general\",\"type\":0},{\"id\":\"13\",\"paren"
      "t_id\":\"10\",\"name\":\"off-topic\",\"type\":0},{\"id\":\"20\",\"pa"
      "rent_id\":null,\"name\":\"Voice Channels\",\"type\":4},{\"id\":\"21"
      "\",\"parent_id\":\"20\",\"name\":\"Lounge\",\"type\":2},{\"id\":\"22"
      "\",\"parent_id\":\"20\",\"name\":\"Study Room 1\",\"type\":2},{\"id\""
      ":\"23\",\"parent_id\":\"20\",\"name\":\"Study Room 2\",\"type\":2}],"
      "\"system_channel_id\":\"12\",\"guild_template_code\":\"r86WWBwTGspb"
      "\"}",
  },
  {
    .label = "Social",
    .body =
      "{\"icon\":null,\"channels\":[{\"id\":\"00\",\"parent_id\":null,\"nam"
      "e\":\"Chat\",\"type\":4},{\"id\":\"01\",\"parent_id\":\"00\",\"name"
      "\":\"introductions\",\"type\":0},{\"id\":\"02\",\"parent_id\":\"00\","
      "\"name\":\"general\",\"type\":0},{\"id\":\"03\",\"parent_id\":\"00\""
      ",\"name\":\"off-topic\",\"type\":0},{\"id\":\"04\",\"parent_id\":\"0"
      "0\",\"name\":\"meetups\",\"type\":0},{\"id\":\"10\",\"parent_id\":nu"
      "ll,\"name\":\"Voice Channels\",\"type\":4},{\"id\":\"11\",\"parent_i"
      "d\":\"10\",\"name\":\"Couches\",\"type\":2},{\"id\":\"12\",\"parent_"
      "id\":\"10\",\"name\":\"Movie Room\",\"type\":2},{\"id\":\"13\",\"par"
      "ent_id\":\"10\",\"name\":\"Study Room\",\"type\":2}],\"system_channe"
      "l_id\":\"02\",\"guild_template_code\":\"AvvtXE3mfbCR\"}",
  },
  {
    .label = "Subject or Major",
    .body =
      "{\"icon\":null,\"channels\":[{\"id\":\"00\",\"parent_id\":null,\"nam"
      "e\":\"Info\",\"type\":4},{\"id\":\"01\",\"parent_id\":\"00\",\"name"
      "\":\"resources\",\"type\":0},{\"id\":\"02\",\"parent_id\":\"00\",\"na"
      "me\":\"help-and-questions\",\"type\":0},{\"id\":\"10\",\"parent_id\""
      ":null,\"name\":\"Chat\",\"type\":4},{\"id\":\"11\",\"parent_id\":\"1"
      "0\",\"name\":\"introductions\",\"type\":0},{\"id\":\"12\",\"parent_i"
      "d\":\"10\",\"name\":\"general\",\"type\":0},{\"id\":\"13\",\"parent_"
      "id\":\"10\",\"name\":\"off-topic\",\"type\":0},{\"id\":\"20\",\"pare"
      "nt_id\":null,\"name\":\"Voice Channels\",\"type\":4},{\"id\":\"21\","
      "\"parent_id\":\"20\",\"name\":\"Lounge\",\"type\":2},{\"id\":\"22\","
      "\"parent_id\":\"20\",\"name\":\"Study Room 1\",\"type\":2},{\"id\":"
      "\"23\",\"parent_id\":\"20\",\"name\":\"Study Room 2\",\"type\":2}],\""
      "system_channel_id\":\"12\",\"guild_template_code\":\"FhmfDR6Arvsc\"}",
  },
  {
    .label = "Dorm",
    .body =
      "{\"icon\":null,\"channels\":[{\"id\":\"00\",\"parent_id\":null,\"nam"
      "e\":\"Info\",\"type\":4},{\"id\":\"01\",\"parent_id\":\"00\",\"name"
      "\":\"dorm-news\",\"type\":0},{\"id\":\"10\",\"parent_id\":null,\"name"
      "\":\"Chat\",\"type\":4},{\"id\":\"11\",\"parent_id\":\"10\",\"name\""
      ":\"introductions\",\"type\":0},{\"id\":\"12\",\"parent_id\":\"10\","
      "\"name\":\"general\",\"type\":0},{\"id\":\"13\",\"parent_id\":\"10\","
      "\"name\":\"off-topic\",\"type\":0},{\"id\":\"20\",\"parent_id\":null"
      ",\"name\":\"Voice Channels\",\"type\":4},{\"id\":\"21\",\"parent_id"
      "\":\"20\",\"name\":\"Lounge\",\"type\":2},{\"id\":\"22\",\"parent_id"
      "\":\"20\",\"name\":\"Movie Room\",\"type\":2},{\"id\":\"23\",\"parent"
      "_id\":\"20\",\"name\":\"Couches\",\"type\":2}],\"system_channel_id\""
      ":\"12\",\"guild_template_code\":\"fkq8xHfrGE58\"}",
  },
};

const size_t hub_template_count =
  sizeof hub_templates / sizeof *hub_templates;

// the channels, roles and codes of discord's create server dialog, its
// school club repeating an id as discord does.
const GuildTemplate guild_templates[] = {
  {
    .label = "Create My Own",
    .icon = "template:create",
    .body =
      "\"channels\":[],\"system_channel_id\":null,\"guild_template_code\":"
      "\"2TffvPucqHkN\"}",
  },
  {
    .label = "Gaming",
    .icon = "template:gaming",
    .body =
      "\"channels\":[{\"id\":\"00\",\"parent_id\":null,\"name\":\"Text Chan"
      "nels\",\"type\":4},{\"id\":\"01\",\"parent_id\":\"00\",\"name\":\"ge"
      "neral\",\"type\":0},{\"id\":\"02\",\"parent_id\":\"00\",\"name\":\"c"
      "lips-and-highlights\",\"type\":0},{\"id\":\"10\",\"parent_id\":null,"
      "\"name\":\"Voice Channels\",\"type\":4},{\"id\":\"11\",\"parent_id\""
      ":\"10\",\"name\":\"Lobby\",\"type\":2},{\"id\":\"12\",\"parent_id\":"
      "\"10\",\"name\":\"Gaming\",\"type\":2}],\"system_channel_id\":\"01\""
      ",\"guild_template_code\":\"hvtBQMfw6uSJ\"}",
  },
  {
    .label = "Friends",
    .icon = "template:friends",
    .body =
      "\"channels\":[{\"id\":\"00\",\"parent_id\":null,\"name\":\"Text Chan"
      "nels\",\"type\":4},{\"id\":\"01\",\"parent_id\":\"00\",\"name\":\"ge"
      "neral\",\"type\":0},{\"id\":\"02\",\"parent_id\":\"00\",\"name\":\"g"
      "ames\",\"type\":0},{\"id\":\"03\",\"parent_id\":\"00\",\"name\":\"mu"
      "sic\",\"type\":0},{\"id\":\"10\",\"parent_id\":null,\"name\":\"Voice"
      " Channels\",\"type\":4},{\"id\":\"11\",\"parent_id\":\"10\",\"name\""
      ":\"Lounge\",\"type\":2},{\"id\":\"12\",\"parent_id\":\"10\",\"name\""
      ":\"Stream Room\",\"type\":2}],\"system_channel_id\":\"01\",\"guild_t"
      "emplate_code\":\"hgM48av5Q69A\"}",
  },
  {
    .label = "Study Group",
    .icon = "template:study",
    .body =
      "\"channels\":[{\"id\":\"00\",\"parent_id\":null,\"name\":\"Informati"
      "on\",\"type\":4},{\"id\":\"01\",\"parent_id\":\"00\",\"name\":\"welc"
      "ome-and-rules\",\"type\":0},{\"id\":\"02\",\"parent_id\":\"00\",\"na"
      "me\":\"notes-resources\",\"type\":0},{\"id\":\"10\",\"parent_id\":nu"
      "ll,\"name\":\"Text Channels\",\"type\":4},{\"id\":\"11\",\"parent_id"
      "\":\"10\",\"name\":\"general\",\"type\":0},{\"id\":\"12\",\"parent_i"
      "d\":\"10\",\"name\":\"homework-help\",\"type\":0},{\"id\":\"13\",\"p"
      "arent_id\":\"10\",\"name\":\"session-planning\",\"type\":0},{\"id\":"
      "\"14\",\"parent_id\":\"10\",\"name\":\"off-topic\",\"type\":0},{\"id"
      "\":\"20\",\"parent_id\":null,\"name\":\"Voice Channels\",\"type\":4}"
      ",{\"id\":\"21\",\"parent_id\":\"20\",\"name\":\"Lounge\",\"type\":2}"
      ",{\"id\":\"22\",\"parent_id\":\"20\",\"name\":\"Study Room 1\",\"typ"
      "e\":2},{\"id\":\"23\",\"parent_id\":\"20\",\"name\":\"Study Room 2\""
      ",\"type\":2}],\"system_channel_id\":\"11\",\"guild_template_code\":"
      "\"FbwUwRp4j8Es\"}",
  },
  {
    .label = "School Club",
    .icon = "template:school_club",
    .body =
      "\"channels\":[{\"id\":\"00\",\"parent_id\":null,\"name\":\"Informati"
      "on\",\"type\":4},{\"id\":\"01\",\"parent_id\":\"00\",\"name\":\"welc"
      "ome-and-rules\",\"type\":0},{\"id\":\"02\",\"parent_id\":\"00\",\"na"
      "me\":\"announcements\",\"type\":0},{\"id\":\"03\",\"parent_id\":\"00"
      "\",\"name\":\"resources\",\"type\":0},{\"id\":\"10\",\"parent_id\":n"
      "ull,\"name\":\"Text Channels\",\"type\":4},{\"id\":\"11\",\"parent_i"
      "d\":\"10\",\"name\":\"general\",\"type\":0},{\"id\":\"12\",\"parent_"
      "id\":\"10\",\"name\":\"meeting-plans\",\"type\":0},{\"id\":\"13\",\""
      "parent_id\":\"10\",\"name\":\"off-topic\",\"type\":0},{\"id\":\"20\""
      ",\"parent_id\":null,\"name\":\"Voice Channels\",\"type\":4},{\"id\":"
      "\"21\",\"parent_id\":\"20\",\"name\":\"Lounge\",\"type\":2},{\"id\":"
      "\"22\",\"parent_id\":\"20\",\"name\":\"Meeting Room 1\",\"type\":2},"
      "{\"id\":\"22\",\"parent_id\":\"20\",\"name\":\"Meeting Room 2\",\"ty"
      "pe\":2}],\"system_channel_id\":\"11\",\"roles\":[{\"id\":\"00\",\"na"
      "me\":\"@everyone\",\"permissions\":\"1720707884502593\"},{\"id\":\"0"
      "1\",\"name\":\"officers (example)\",\"mentionable\":true,\"hoist\":t"
      "rue,\"permissions\":\"1720709360909911\",\"color\":3066993}],\"guild"
      "_template_code\":\"Ctg7PUHcQmZu\"}",
  },
  {
    .label = "Local Community",
    .icon = "template:local_community",
    .body =
      "\"channels\":[{\"id\":\"00\",\"parent_id\":null,\"name\":\"Informati"
      "on\",\"type\":4},{\"id\":\"01\",\"parent_id\":\"00\",\"name\":\"welc"
      "ome-and-rules\",\"type\":0},{\"id\":\"02\",\"parent_id\":\"00\",\"na"
      "me\":\"announcements\",\"type\":0},{\"id\":\"03\",\"parent_id\":\"00"
      "\",\"name\":\"resources\",\"type\":0},{\"id\":\"10\",\"parent_id\":n"
      "ull,\"name\":\"Text Channels\",\"type\":4},{\"id\":\"11\",\"parent_i"
      "d\":\"10\",\"name\":\"general\",\"type\":0},{\"id\":\"12\",\"parent_"
      "id\":\"10\",\"name\":\"meeting-plans\",\"type\":0},{\"id\":\"13\",\""
      "parent_id\":\"10\",\"name\":\"off-topic\",\"type\":0},{\"id\":\"20\""
      ",\"parent_id\":null,\"name\":\"Voice Channels\",\"type\":4},{\"id\":"
      "\"21\",\"parent_id\":\"20\",\"name\":\"Lounge\",\"type\":2},{\"id\":"
      "\"22\",\"parent_id\":\"20\",\"name\":\"Meeting Room\",\"type\":2}],"
      "\"system_channel_id\":\"11\",\"guild_template_code\":\"64UDvRNCC52Y"
      "\"}",
  },
  {
    .label = "Artists & Creators",
    .icon = "template:creators",
    .body =
      "\"channels\":[{\"id\":\"00\",\"parent_id\":null,\"name\":\"Informati"
      "on\",\"type\":4},{\"id\":\"01\",\"parent_id\":\"00\",\"name\":\"welc"
      "ome-and-rules\",\"type\":0},{\"id\":\"02\",\"parent_id\":\"00\",\"na"
      "me\":\"announcements\",\"type\":0},{\"id\":\"10\",\"parent_id\":null"
      ",\"name\":\"Text Channels\",\"type\":4},{\"id\":\"11\",\"parent_id\""
      ":\"10\",\"name\":\"general\",\"type\":0},{\"id\":\"12\",\"parent_id"
      "\":\"10\",\"name\":\"events\",\"type\":0},{\"id\":\"13\",\"parent_id"
      "\":\"10\",\"name\":\"ideas-and-feedback\",\"type\":0},{\"id\":\"20\""
      ",\"parent_id\":null,\"name\":\"Voice Channels\",\"type\":4},{\"id\":"
      "\"21\",\"parent_id\":\"20\",\"name\":\"Lounge\",\"type\":2},{\"id\":"
      "\"22\",\"parent_id\":\"20\",\"name\":\"Community Hangout\",\"type\":"
      "2},{\"id\":\"23\",\"parent_id\":\"20\",\"name\":\"Stream Room\",\"ty"
      "pe\":2}],\"system_channel_id\":\"11\",\"guild_template_code\":\"6exd"
      "zMgjZgah\"}",
  },
};

const size_t guild_template_count =
  sizeof guild_templates / sizeof *guild_templates;

static const unsigned char CREATE[] = {
#embed "../../assets/discord/templates/create.svg"
};
static const unsigned char GAMING[] = {
#embed "../../assets/discord/templates/gaming.svg"
};
static const unsigned char FRIENDS[] = {
#embed "../../assets/discord/templates/friends.svg"
};
static const unsigned char STUDY[] = {
#embed "../../assets/discord/templates/study.svg"
};
static const unsigned char SCHOOL_CLUB[] = {
#embed "../../assets/discord/templates/school_club.svg"
};
static const unsigned char LOCAL_COMMUNITY[] = {
#embed "../../assets/discord/templates/local_community.svg"
};
static const unsigned char CREATORS[] = {
#embed "../../assets/discord/templates/creators.svg"
};

static const struct {
  const char *name;
  const unsigned char *bytes;
  size_t len;
} ICONS[] = {
  {"create", CREATE, sizeof CREATE},
  {"gaming", GAMING, sizeof GAMING},
  {"friends", FRIENDS, sizeof FRIENDS},
  {"study", STUDY, sizeof STUDY},
  {"school_club", SCHOOL_CLUB, sizeof SCHOOL_CLUB},
  {"local_community", LOCAL_COMMUNITY, sizeof LOCAL_COMMUNITY},
  {"creators", CREATORS, sizeof CREATORS},
};

const char *template_svg(const char *url, size_t *len) {
  if (strncmp(url, "template:", 9) != 0) {
    return nullptr;
  }

  for (size_t i = 0; i < sizeof ICONS / sizeof *ICONS; i++) {
    if (strcmp(url + 9, ICONS[i].name) == 0) {
      *len = ICONS[i].len;
      return (const char *)ICONS[i].bytes;
    }
  }

  return nullptr;
}
