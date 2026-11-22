#include "discord/templates.h"

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
