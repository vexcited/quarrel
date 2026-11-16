#ifndef QUARREL_UI_COMPONENTS_H
#define QUARREL_UI_COMPONENTS_H

#include "app.h"

void login_screen(App *app);
void connecting_screen(App *app);

void top_bar(App *app);
void guild_bar(App *app);
void channel_list(App *app, Guild *guild);
void private_list(App *app);
void user_panel(App *app);

void chat(App *app, Channel *channel);
void message_list(App *app, Channel *channel);
void composer(App *app, Channel *channel);
void member_list(App *app, Channel *channel);
void friends_page(App *app);

#endif
