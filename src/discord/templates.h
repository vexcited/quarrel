#ifndef QUARREL_DISCORD_TEMPLATES_H
#define QUARREL_DISCORD_TEMPLATES_H

#include <stddef.h>

// what a server made for a student hub starts from, as discord's client
// sends it.
typedef struct {
  const char *label;
  // the json of POST /guilds without the name, from `{"icon":` on.
  const char *body;
} HubTemplate;

// "Create New Server" first, then the templates in discord's order.
extern const HubTemplate hub_templates[];
extern const size_t hub_template_count;

#endif
