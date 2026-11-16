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
// the saved accounts over `anchor`, while app->accounts_open. a press on
// `toggle` is left to what opened it.
void account_switcher(App *app, civue_rect anchor, civue_rect toggle);
// the saved accounts under the login form, nothing when there is none.
void account_choices(App *app);

void chat(App *app, Channel *channel);
void message_list(App *app, Channel *channel);
void composer(App *app, Channel *channel);
void member_list(App *app, Channel *channel);
void friends_page(App *app);

#endif
