#include "look.h"

#include <civue/dsl.h>

Look look_dark() {
  return (Look){
    .frame = rgb(0x121214),
    .frame_border = rgba(0x97979f1f),
    .chat = rgb(0x1a1a1e),
    .panel = rgb(0x202024),
    .surface = rgb(0x242429),
    .surface_high = rgb(0x2c2d32),
    .input = rgb(0x222327),
    .text = rgb(0xefeff1),
    .strong = rgb(0xfbfbfb),
    .subtle = rgb(0xabacb2),
    .muted = rgb(0x96979e),
    .faint = rgb(0x81828a),
    .link = rgb(0x4d96ee),
    .brand = rgb(0x5865f2),
    .brand_hover = rgb(0x4452bb),
    .brand_active = rgb(0x3a48a3),
    .danger = rgb(0xd22d39),
    .critical_text = rgb(0xf87e7a),
    .positive = rgb(0x008545),
    .online = rgb(0x3d9e60),
    .idle = rgb(0xffcb6e),
    .dnd = rgb(0xdc4247),
    .offline = rgb(0x9d9ea5),
    .hover = rgba(0x97979f1f),
    .selected = rgba(0x97979f33),
    .pressed = rgba(0x97979f29),
    .border = rgba(0x97979f1f),
    .border_strong = rgba(0x97979f33),
    .message_hover = rgba(0x97979f0f),
    .mentioned = rgba(0xf8a30014),
    .mentioned_bar = rgb(0xf8a300),
    .mention_bg = rgba(0x5865f23d),
    .mention_text = rgb(0xa9bbff),
    .code_bg = rgba(0x97979f1f),
    .spoiler = rgb(0x41424a),
    .thumb = rgb(0x5f606a),
  };
}
