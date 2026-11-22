#ifndef QUARREL_UI_FORMS_H
#define QUARREL_UI_FORMS_H

#include "app.h"

/* ========================================================================== */
/* the controls of discord's settings and dialogs, drawn as its stylesheets   */
/* draw them.                                                                 */
/* ========================================================================== */

// discord's green, of drop lines and of its save button.
constexpr uint32_t FORM_GREEN = 0x3d9e60;
// --text-brand, of the selected tab.
constexpr uint32_t FORM_TEXT_BRAND = 0x798df9;

typedef enum : uint8_t {
  BUTTON_BRAND,
  BUTTON_DANGER,
  BUTTON_GREEN,
  BUTTON_SECONDARY,
  // a gray button with red text.
  BUTTON_DANGER_SECONDARY,
  // text alone, underlined on hover.
  BUTTON_LINK,
} ButtonKind;

// a button 40 points tall, true when pressed. a disabled one is dimmed and
// never pressed.
bool form_button(App *app, civue_id id, const char *label, ButtonKind kind,
                 bool disabled);
// the same 32 tall.
bool form_button_small(App *app, civue_id id, const char *label,
                       ButtonKind kind, bool disabled);

// a switch 48 by 24, true when flipped.
bool form_switch(App *app, civue_id id, bool on, bool disabled);
// a title and what it does, the switch at the right. true when flipped.
bool form_switch_row(App *app, civue_id id, const char *title,
                     const char *about, bool on, bool disabled);
// a box ticked or not, 20 square, true when pressed.
bool form_checkbox(App *app, civue_id id, bool on);
// a choice of a group: a round mark, a title and what it means under it,
// with an icon before the title when `glyph_icon` is not null. true when
// pressed.
bool form_radio(App *app, civue_id id, const civue_icon *glyph_icon,
                const char *title, const char *about, bool on, bool disabled);

// the title of a section of a page, 20 points medium, and the line under
// it.
void form_legend(const Look *k, const char *title, const char *about);
// a title over a field, 16 points medium.
void form_label(const Look *k, const char *title);
// the small gray line under a field.
void form_note(const Look *k, const char *line);
// the title of a page, 20 points semibold, and the line under it.
void form_heading(const Look *k, const char *title, const char *about);
// the line between the sections of a page.
void form_divider(const Look *k);

// a text field 40 tall over `*value`, `prefix` drawn before the text when
// not null, at most `most` bytes when not 0. what it did this frame.
civue_field_response form_input(App *app, civue_id id, char **value,
                                const char *placeholder, const char *prefix,
                                size_t most);
// puts `text` in the field `id` over `*value`, the field's own copy too
// while it edits.
void form_input_set(civue_id id, char **value, const char *text);
// several lines, `lines` tall.
civue_field_response form_area(App *app, civue_id id, char **value,
                               const char *placeholder, int lines,
                               size_t most);

/* ========================================================================== */
/* dropdowns. the box opens a list under itself, drawn over everything at    */
/* the end of the frame by form_dropdowns.                                    */
/* ========================================================================== */

typedef struct {
  // the box whose list is open, 0 for none, and where it is.
  civue_id open;
  civue_rect box;
  // the choices it was opened with.
  char **names;
  size_t count;
  int current;
  // a choice made in the list, for the box that opened it to take.
  civue_id chosen_for;
  int chosen;
} Dropdown;

// a box naming `names[current]`, or `placeholder` for -1. the index chosen
// in its list, -1 while none was.
int form_select(App *app, Dropdown *d, civue_id id, const char *const *names,
                size_t count, int current, const char *placeholder,
                bool disabled);
// the open list. call once per frame, after every box.
void form_dropdowns(App *app, Dropdown *d);
void form_dropdown_close(Dropdown *d);

/* ========================================================================== */
/* the notice of unsaved changes, and the shake of the app when the user      */
/* tries to leave them.                                                       */
/* ========================================================================== */

typedef enum : uint8_t {
  NOTICE_NONE,
  NOTICE_RESET,
  NOTICE_SAVE,
} NoticeAction;

typedef struct {
  // when the user last tried to leave, 0 for never, and how hard the app
  // shakes the next time.
  double nagged_at;
  float intensity;
} Nag;

// the user tried to leave unsaved changes: the app shakes, a little harder
// each time, and the notice turns red for a second.
void form_nag(Nag *n);
// they may leave: the next shake starts soft again.
void form_nag_reset(Nag *n);
// how far the app is shaken this frame.
civue_vec2 form_shake(const Nag *n);

// "Careful, you have unsaved changes!" at the foot of `region` while
// `shown`, with `error` in place of the words when not null.
NoticeAction form_notice(App *app, civue_rect region, bool shown, bool saving,
                         const char *error, const Nag *n);

/* ========================================================================== */
/* dialogs.                                                                   */
/* ========================================================================== */

// a dialog asking to confirm something that cannot be undone, `danger`
// for a red button. 1 for yes, -1 for no, 0 while it waits.
int form_confirm(App *app, civue_id id, const char *title, const char *body,
                 const char *yes, bool danger, bool busy, const char *error);

#endif
