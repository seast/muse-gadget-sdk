/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "muse_clock_view.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "sdkconfig.h"

#include "muse_clock_art.h"
#include "muse_lang.h"
#if CONFIG_MUSE_WEATHER
#include "muse_weather.h"
#endif

#if CONFIG_MUSE_WEATHER_FAHRENHEIT
#define UNIT_MARK "°F"
#else
#define UNIT_MARK "°C"
#endif

#if LV_FONT_MONTSERRAT_12
#define FONT_TAG_SMALL (&lv_font_montserrat_12)
#else
#define FONT_TAG_SMALL (&lv_font_montserrat_14)
#endif

#define COLOR_CARD 0x1f222a
#define COLOR_BORDER 0x343843
#define COLOR_TEXT 0xe8eaf0
#define COLOR_DIM 0x8b90a0
#define COLOR_CITY 0x8fc3ff
#define COLOR_WEEK 0xf0a030
#define COLOR_MINUTES 0xf5a033
#define COLOR_SECONDS 0x4cc46a
#define COLOR_TAG 0x9fd8ff
#define COLOR_ON_TAG 0x14202c

static lv_obj_t *s_root, *s_slot;
static lv_obj_t *s_week, *s_date, *s_city, *s_icon;
static lv_obj_t *s_hh, *s_mm, *s_ss, *s_tag;
static lv_obj_t *s_temp, *s_range, *s_humidity;
static time_t s_shown_t;

static lv_obj_t *card(lv_obj_t *parent, int x, int y, int w, int h, uint32_t bg)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_size(c, w, h);
    lv_obj_set_style_bg_color(c, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(c, lv_color_hex(COLOR_BORDER), 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_radius(c, 10, 0);
    lv_obj_set_style_clip_corner(c, true, 0);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return c;
}

static lv_obj_t *box(lv_obj_t *parent, lv_flex_flow_t flow)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(b, flow);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return b;
}

static lv_obj_t *text(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *s)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, s);
    return l;
}

static lv_obj_t *tag(lv_obj_t *parent, const lv_font_t *font, uint32_t bg, const char *s)
{
    lv_obj_t *l = text(parent, font, COLOR_ON_TAG, s);
    lv_obj_set_style_bg_color(l, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(l, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(l, 3, 0);
    lv_obj_set_style_pad_hor(l, 4, 0);
    lv_obj_set_style_pad_ver(l, 1, 0);
    return l;
}

static lv_obj_t *reading(lv_obj_t *parent, const lv_image_dsc_t *badge, const lv_font_t *font)
{
    lv_obj_t *row = box(parent, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 10, 0);
    lv_obj_t *img = lv_image_create(row);
    lv_image_set_src(img, badge);
    return text(row, font, COLOR_TEXT, "--");
}

void muse_clock_view_build(lv_obj_t *parent, int w, int h)
{
    bool tall = h > w;
    const int m = 6;
    int pad = tall ? 10 : 12;
    int top_h = tall ? 188 : 150;
    int low_y = m + top_h + m;
    int low_h = h - low_y - m;
    int muse_w = tall ? 86 : 76;
    int left_w = w - 3 * m - muse_w;
    const lv_font_t *row_font = tall ? &lv_font_montserrat_16 : &lv_font_montserrat_20;
    const lv_font_t *small = tall ? FONT_TAG_SMALL : &lv_font_montserrat_14;

    s_root = lv_obj_create(parent);
    lv_obj_remove_style_all(s_root);
    lv_obj_set_size(s_root, w, h);
    lv_obj_set_style_bg_color(s_root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *top = card(s_root, m, m, w - 2 * m, top_h, COLOR_CARD);
    lv_obj_t *head = box(top, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(head, 6, 0);
    lv_obj_set_pos(head, pad, pad);
    s_week = tag(head, small, COLOR_WEEK, "---");
    s_date = text(head, row_font, COLOR_TEXT, "----/--/--");
    s_city = text(top, row_font, COLOR_CITY, "");
    lv_obj_set_width(s_city, w - 2 * m - 2 * pad - 48 - 6);
    lv_label_set_long_mode(s_city, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_pos(s_city, pad, pad + (tall ? 26 : 30));
    s_icon = lv_image_create(top);
    lv_obj_align(s_icon, LV_ALIGN_TOP_RIGHT, -pad + 2, pad - 4);

    const lv_font_t *big = tall ? &muse_font_clock_64 : &muse_font_clock_84;
    int below = pad + (tall ? 26 : 30) + lv_font_get_line_height(row_font);
    lv_obj_t *time_row = box(top, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(time_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_pos(time_row, pad - 2, below + (top_h - below - lv_font_get_line_height(big)) / 2);
    s_hh = text(time_row, big, 0xffffff, "--");
    text(time_row, big, 0xffffff, ":");
    s_mm = text(time_row, big, COLOR_MINUTES, "--");
    lv_obj_t *side = box(time_row, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(side, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_left(side, 6, 0);
    lv_obj_set_style_pad_row(side, tall ? 4 : 6, 0);
    s_tag = tag(side, small, COLOR_TAG, "");
    lv_obj_add_flag(s_tag, LV_OBJ_FLAG_HIDDEN);
    s_ss = text(side, &muse_font_clock_40, COLOR_SECONDS, "--");

    lv_obj_t *low = card(s_root, m, low_y, left_w, low_h, COLOR_CARD);
    lv_obj_set_flex_flow(low, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(low, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_left(low, pad, 0);
    s_temp = reading(low, &muse_clock_badge_temp, &lv_font_montserrat_20);
    s_range = text(lv_obj_get_parent(s_temp), &lv_font_montserrat_14, COLOR_DIM, "");
    lv_obj_set_flag(s_range, LV_OBJ_FLAG_HIDDEN, tall);
    s_humidity = reading(low, &muse_clock_badge_humidity, &lv_font_montserrat_20);

    s_slot = card(s_root, w - m - muse_w, low_y, muse_w, low_h, 0x000000);
    s_shown_t = 0;
}

void muse_clock_view_forget(void)
{
    s_root = s_slot = NULL;
    s_week = s_date = s_city = s_icon = NULL;
    s_hh = s_mm = s_ss = s_tag = NULL;
    s_temp = s_range = s_humidity = NULL;
}

lv_obj_t *muse_clock_view_root(void)
{
    return s_root;
}

lv_obj_t *muse_clock_view_muse_slot(void)
{
    return s_slot;
}

static void set_text(lv_obj_t *l, const char *s)
{
    if (strcmp(lv_label_get_text(l), s) != 0) {
        lv_label_set_text(l, s);
    }
}

#if CONFIG_MUSE_WEATHER
static const char *short_condition(int code)
{
    if (code == 0) return "Clear";
    if (code <= 2) return "Fair";
    if (code == 3) return "Cloudy";
    if (code == 45 || code == 48) return "Fog";
    if (code >= 51 && code <= 57) return "Drizzle";
    if ((code >= 61 && code <= 67) || (code >= 80 && code <= 82)) return "Rain";
    if ((code >= 71 && code <= 77) || code == 85 || code == 86) return "Snow";
    if (code >= 95) return "Storm";
    return "";
}

static const lv_image_dsc_t *icon_for(int code, bool day)
{
    if (code <= 1) return day ? &muse_clock_icon_sun : &muse_clock_icon_moon;
    if (code == 2) return day ? &muse_clock_icon_sun_cloud : &muse_clock_icon_moon_cloud;
    if (code == 45 || code == 48) return &muse_clock_icon_fog;
    if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82)) return &muse_clock_icon_rain;
    if ((code >= 71 && code <= 77) || code == 85 || code == 86) return &muse_clock_icon_snow;
    if (code >= 95) return &muse_clock_icon_storm;
    return &muse_clock_icon_cloud;
}

static void update_weather(void)
{
    muse_weather_t w;
    if (!muse_weather_now(&w)) {
        return;
    }
    if (strcmp(lv_label_get_text(s_city), w.place) != 0) {
        lv_obj_set_style_text_font(s_city, muse_lang_label_font(w.place, lv_obj_get_style_text_font(s_date, 0)), 0);
        lv_label_set_text(s_city, w.place);
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "%ld" UNIT_MARK, lroundf(w.temp));
    set_text(s_temp, buf);
    if (w.humidity >= 0) {
        snprintf(buf, sizeof(buf), "%d%%", w.humidity);
        set_text(s_humidity, buf);
    }
    if (!isnan(w.high) && !isnan(w.low)) {
        snprintf(buf, sizeof(buf), "H %ld°  L %ld°", lroundf(w.high), lroundf(w.low));
        set_text(s_range, buf);
    }
    const char *cond = short_condition(w.code);
    set_text(s_tag, cond);
    lv_obj_set_flag(s_tag, LV_OBJ_FLAG_HIDDEN, !cond[0]);
    const lv_image_dsc_t *icon = icon_for(w.code, w.day);
    if (lv_image_get_src(s_icon) != icon) {
        lv_image_set_src(s_icon, icon);
    }
}
#endif

void muse_clock_view_update(void)
{
    if (!s_root) {
        return;
    }
    time_t t = time(NULL);
    if (t == s_shown_t) {
        return;
    }
    s_shown_t = t;
    static const char *const WEEK[7] = { "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT" };
    struct tm tm;
    char buf[16];
    if (localtime_r(&t, &tm) && tm.tm_year >= 2024 - 1900) {
        snprintf(buf, sizeof(buf), "%02d", tm.tm_hour);
        set_text(s_hh, buf);
        snprintf(buf, sizeof(buf), "%02d", tm.tm_min);
        set_text(s_mm, buf);
        snprintf(buf, sizeof(buf), "%02d", tm.tm_sec);
        set_text(s_ss, buf);
        set_text(s_week, WEEK[tm.tm_wday]);
        strftime(buf, sizeof(buf), "%Y/%m/%d", &tm);
        set_text(s_date, buf);
    }
#if CONFIG_MUSE_WEATHER
    update_weather();
#endif
}
