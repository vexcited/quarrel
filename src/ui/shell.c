#include "components.h"

#include <civue/dsl.h>

void top_bar(App *app) {
  const Look *k = &app->look;
  const Guild *guild = app_guild(app);

  div(flex, items_center, justify_center, gap(2), flex_shrink_0, w(full),
      h(px(TOP_BAR_HEIGHT)), text_size(px(14)), line_height(px(18)),
      font_weight(medium), text_color(k->text)) {
    if (guild) {
      text(guild->name ? guild->name : "", max_w(1_2), truncate);
    }
    else {
      icon(lucide, users, size(4), text_color(k->subtle));
      text("Direct Messages");
    }
  }
}
