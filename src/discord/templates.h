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

// what a server made from the dialog adding one starts from.
typedef struct {
  const char *label;
  // an url civue's images read with template_svg, never fetched.
  const char *icon;
  // the json of POST /guilds after the name and icon, from `"channels":` on.
  const char *body;
} GuildTemplate;

// "Create My Own" first, then the templates in discord's order.
extern const GuildTemplate guild_templates[];
extern const size_t guild_template_count;

// the svg of a template's icon, null for another url.
const char *template_svg(const char *url, size_t *len);

#endif
