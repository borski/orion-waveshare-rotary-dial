#pragma once
#include "lvgl.h"

/*
 * Boost icons for the dial face, from dial_font_icons_16/20/64.c (Bootstrap
 * Icons, MIT). UTF-8 encodings of Bootstrap's private-use codepoints -- LVGL takes
 * icon glyphs as ordinary text, so these are plain string literals.
 */
LV_FONT_DECLARE(dial_font_icons_16)   // status chip (matches montserrat_16 text)
LV_FONT_DECLARE(dial_font_icons_20)   // arc-end boost icons
LV_FONT_DECLARE(dial_font_icons_64)   // hero numeral slot while boosting (flame/snow3 only)

#define DIAL_ICON_FLAME  "\xEF\x9F\xB6"   // U+F7F6 fire      -> boost heat
#define DIAL_ICON_SNOW   "\xEF\x95\xAD"   // U+F56D snow      -> boost cool
#define DIAL_ICON_SNOW3  "\xEF\x95\xAF"   // U+F56F snow3     (alternate flake)
