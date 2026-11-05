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
#include "muse_pixel.h"
#include "muse_settings.h"
#include "muse_wifi.h"
#if CONFIG_MUSE_WEATHER
#include "muse_weather.h"
#endif

#ifndef CONFIG_MUSE_HIDE_REPLY_CAPTIONS
#define CONFIG_MUSE_HIDE_REPLY_CAPTIONS 0
#endif

#if CONFIG_MUSE_WEATHER_FAHRENHEIT
#define UNIT_MARK "°F"
#else
#define UNIT_MARK "°C"
#endif

#if LV_FONT_MONTSERRAT_12
#define FONT_SMALL (&lv_font_montserrat_12)
#else
#define FONT_SMALL (&lv_font_montserrat_14)
#endif

#define COLOR_CARD 0x1f222a
#define COLOR_BORDER 0x343843
#define COLOR_TEXT 0xe8eaf0
#define COLOR_DIM 0x8b90a0
#define COLOR_CITY 0x8fc3ff
#define COLOR_WEEK 0xf0a030
#define COLOR_MINUTES 0xf5a033
#define COLOR_CHARGING 0x4cc46a
#define COLOR_TAG 0x9fd8ff
#define COLOR_ON_TAG 0x14202c
#define COLOR_MUSE 0xb59bff
#define COLOR_OFF 0x3a3f4b
#define COLOR_LOW 0xe64c4c
#define MUSE_PX (MUSE_PX_W * 2)
#define MUSE_W 140
#define BATTERY_W 22
#define BATTERY_INNER (BATTERY_W - 2 - 4)
#define LEVEL_W 96

static lv_obj_t *s_root, *s_slot;
static lv_obj_t *s_week, *s_date, *s_city, *s_source, *s_hh, *s_mm;
static lv_obj_t *s_icon, *s_temp, *s_tag, *s_range, *s_humidity;
static lv_obj_t *s_speaker, *s_bars[4], *s_battery, *s_battery_fill, *s_usb;
static lv_obj_t *s_level, *s_level_fill, *s_state, *s_caption;
static time_t s_shown_t;
static int s_shown_lit, s_shown_fill, s_shown_speaker, s_shown_level, s_shown_mode;
static uint32_t s_shown_battery_color, s_caption_version;
static const char *s_shown_name;
static char s_caption_text[MUSE_CAPTION_MAX];

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

static lv_obj_t *tag(lv_obj_t *parent, uint32_t bg, const char *s)
{
    lv_obj_t *l = text(parent, FONT_SMALL, COLOR_ON_TAG, s);
    lv_obj_set_style_bg_color(l, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(l, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(l, 3, 0);
    lv_obj_set_style_pad_hor(l, 4, 0);
    lv_obj_set_style_pad_ver(l, 1, 0);
    return l;
}

static lv_obj_t *rect(lv_obj_t *parent, int w, int h, uint32_t color)
{
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, w, h);
    lv_obj_set_style_bg_color(r, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(r, 1, 0);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return r;
}

static lv_obj_t *row(lv_obj_t *parent, int gap)
{
    lv_obj_t *r = box(parent, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r, gap, 0);
    return r;
}

static void build_clock(lv_obj_t *c, int w)
{
    const int pad = 8;
    lv_obj_t *head = row(c, 6);
    lv_obj_set_pos(head, pad, pad);
    s_week = tag(head, COLOR_WEEK, "---");
    s_date = text(head, &lv_font_montserrat_16, COLOR_TEXT, "----/--/--");

    lv_obj_t *place = row(c, 6);
    lv_obj_set_pos(place, pad, pad + 24);
    s_city = text(place, &lv_font_montserrat_16, COLOR_CITY, "");
    lv_obj_set_style_max_width(s_city, w - 2 * pad - 6 - 20, 0);
    lv_label_set_long_mode(s_city, LV_LABEL_LONG_MODE_DOTS);
    s_source = tag(place, COLOR_DIM, "");
    lv_obj_add_flag(s_source, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *time_row = box(c, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(time_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_align(time_row, LV_ALIGN_BOTTOM_MID, 0, -2);
    s_hh = text(time_row, &muse_font_clock_64, 0xffffff, "--");
    text(time_row, &muse_font_clock_64, 0xffffff, ":");
    s_mm = text(time_row, &muse_font_clock_64, COLOR_MINUTES, "--");
}

static void build_humidity(lv_obj_t *parent)
{
    lv_obj_t *r = row(parent, 2);
    lv_obj_t *drop = lv_image_create(r);
    lv_image_set_src(drop, &muse_clock_badge_humidity);
    lv_image_set_scale(drop, 128);
    lv_obj_set_size(drop, 16, 16);
    lv_image_set_inner_align(drop, LV_IMAGE_ALIGN_CENTER);
    s_humidity = text(r, FONT_SMALL, COLOR_TEXT, "--");
}

static void build_weather(lv_obj_t *c, bool tall)
{
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(c, 6, 0);
    if (tall) {
        lv_obj_set_flex_align(c, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        s_icon = lv_image_create(c);
        s_temp = text(c, &lv_font_montserrat_20, COLOR_TEXT, "--");
        s_tag = tag(c, COLOR_TAG, "");
        s_range = text(c, FONT_SMALL, COLOR_DIM, "");
        build_humidity(c);
    } else {
        lv_obj_set_flex_align(c, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_t *top = row(c, 8);
        s_icon = lv_image_create(top);
        s_temp = text(top, &lv_font_montserrat_28, COLOR_TEXT, "--");
        lv_obj_t *low = row(c, 6);
        s_tag = tag(low, COLOR_TAG, "");
        build_humidity(low);
        s_range = text(low, FONT_SMALL, COLOR_DIM, "");
    }
    lv_obj_add_flag(s_tag, LV_OBJ_FLAG_HIDDEN);
}

static void build_status(lv_obj_t *parent)
{
    lv_obj_t *r = row(parent, 8);
    s_speaker = text(r, &lv_font_montserrat_14, COLOR_TEXT, "");
    lv_obj_t *bars = box(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bars, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(bars, 2, 0);
    for (int i = 0; i < 4; i++) {
        s_bars[i] = rect(bars, 3, 4 + 3 * i, COLOR_OFF);
    }
    s_battery = row(r, 0);
    lv_obj_t *body = rect(s_battery, BATTERY_W, 12, 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(body, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_set_style_border_width(body, 1, 0);
    lv_obj_set_style_radius(body, 2, 0);
    lv_obj_set_style_pad_all(body, 2, 0);
    s_battery_fill = rect(body, 0, lv_pct(100), COLOR_TEXT);
    rect(s_battery, 2, 5, COLOR_TEXT);
    lv_obj_add_flag(s_battery, LV_OBJ_FLAG_HIDDEN);
    s_usb = text(r, FONT_SMALL, COLOR_DIM, "");
}

static void build_muse(lv_obj_t *c, bool tall)
{
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_top(c, tall ? 4 : 6, 0);
    lv_obj_set_style_pad_hor(c, 6, 0);
    lv_obj_set_style_pad_row(c, tall ? 1 : 2, 0);
    build_status(c);
    s_slot = lv_obj_create(c);
    lv_obj_remove_style_all(s_slot);
    lv_obj_set_size(s_slot, MUSE_PX, MUSE_PX);
    lv_obj_remove_flag(s_slot, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    s_level = rect(c, LEVEL_W, 3, COLOR_OFF);
    lv_obj_set_style_bg_opa(s_level, LV_OPA_TRANSP, 0);
    s_level_fill = rect(s_level, 0, 3, COLOR_MUSE);
    s_state = text(c, &lv_font_montserrat_14, COLOR_DIM, "");
    lv_obj_set_width(s_state, lv_pct(100));
    lv_obj_set_style_text_align(s_state, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_state, LV_LABEL_LONG_MODE_DOTS);
    s_caption = text(c, FONT_SMALL, COLOR_TEXT, "");
    lv_obj_set_width(s_caption, lv_pct(100));
    lv_obj_set_flex_grow(s_caption, 1);
    lv_obj_set_style_text_align(s_caption, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_caption, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_flag(s_caption, LV_OBJ_FLAG_HIDDEN, tall);
}

void muse_clock_view_build(lv_obj_t *parent, int w, int h)
{
    bool tall = h > w;
    const int m = 6;

    s_root = lv_obj_create(parent);
    lv_obj_remove_style_all(s_root);
    lv_obj_set_size(s_root, w, h);
    lv_obj_set_style_bg_color(s_root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *clock, *weather, *muse;
    int clock_w;
    if (tall) {
        int clock_h = 128, low_y = 2 * m + clock_h, low_h = h - low_y - m;
        clock_w = w - 2 * m;
        clock = card(s_root, m, m, clock_w, clock_h, COLOR_CARD);
        weather = card(s_root, m, low_y, w - 3 * m - MUSE_W, low_h, COLOR_CARD);
        muse = card(s_root, w - m - MUSE_W, low_y, MUSE_W, low_h, 0x000000);
    } else {
        int clock_h = 132;
        clock_w = w - 3 * m - MUSE_W;
        clock = card(s_root, m, m, clock_w, clock_h, COLOR_CARD);
        weather = card(s_root, m, 2 * m + clock_h, clock_w, h - 3 * m - clock_h, COLOR_CARD);
        muse = card(s_root, 2 * m + clock_w, m, MUSE_W, h - 2 * m, 0x000000);
    }
    build_clock(clock, clock_w);
    build_weather(weather, tall);
    build_muse(muse, tall);

    s_shown_t = 0;
    s_shown_lit = s_shown_fill = s_shown_speaker = s_shown_level = s_shown_mode = -1;
    s_shown_battery_color = 0;
    s_shown_name = NULL;
    s_caption_version = 0;
    s_caption_text[0] = '\0';
}

void muse_clock_view_forget(void)
{
    s_root = s_slot = NULL;
    s_week = s_date = s_city = s_source = s_hh = s_mm = NULL;
    s_icon = s_temp = s_tag = s_range = s_humidity = NULL;
    s_speaker = s_battery = s_battery_fill = s_usb = NULL;
    s_level = s_level_fill = s_state = s_caption = NULL;
    memset(s_bars, 0, sizeof(s_bars));
}

lv_obj_t *muse_clock_view_root(void)
{
    return s_root;
}

lv_obj_t *muse_clock_view_muse_slot(void)
{
    return s_slot;
}

int muse_clock_view_muse_px(void)
{
    return MUSE_PX;
}

static void set_text(lv_obj_t *l, const char *s)
{
    if (strcmp(lv_label_get_text(l), s) != 0) {
        lv_label_set_text(l, s);
    }
}

static void update_status(void)
{
    int speaker = muse_settings_speaker_on();
    if (speaker != s_shown_speaker) {
        s_shown_speaker = speaker;
        lv_label_set_text(s_speaker, speaker ? LV_SYMBOL_VOLUME_MAX : LV_SYMBOL_MUTE);
        lv_obj_set_style_text_color(s_speaker, lv_color_hex(speaker ? COLOR_TEXT : COLOR_LOW), 0);
    }
    muse_wifi_status_t w;
    muse_wifi_status(&w);
    int lit = w.state != MUSE_WIFI_CONNECTED ? 0
              : w.rssi >= -55                ? 4
              : w.rssi >= -65                ? 3
              : w.rssi >= -75                ? 2
                                             : 1;
    if (lit != s_shown_lit) {
        s_shown_lit = lit;
        for (int i = 0; i < 4; i++) {
            lv_obj_set_style_bg_color(s_bars[i], lv_color_hex(i < lit ? COLOR_TEXT : COLOR_OFF), 0);
        }
    }

    muse_power_t p = muse_state_power();
    if (p.battery_pct < 0) {
        lv_obj_add_flag(s_battery, LV_OBJ_FLAG_HIDDEN);
        set_text(s_usb, p.usb ? "USB" : "");
        return;
    }
    lv_obj_remove_flag(s_battery, LV_OBJ_FLAG_HIDDEN);
    set_text(s_usb, "");
    int fill = p.battery_pct > 0 ? LV_MAX(1, BATTERY_INNER * p.battery_pct / 100) : 0;
    if (fill != s_shown_fill) {
        s_shown_fill = fill;
        lv_obj_set_width(s_battery_fill, fill);
    }
    uint32_t color = p.charging ? COLOR_CHARGING : p.battery_pct <= 15 ? COLOR_LOW : COLOR_TEXT;
    if (color != s_shown_battery_color) {
        s_shown_battery_color = color;
        lv_obj_set_style_bg_color(s_battery_fill, lv_color_hex(color), 0);
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
        lv_obj_set_style_text_font(s_city, muse_lang_label_font(w.place, &lv_font_montserrat_16), 0);
        lv_label_set_text(s_city, w.place);
    }
    static const struct {
        const char *mark;
        uint32_t color;
    } SOURCES[] = {
        [MUSE_WEATHER_FROM_MUSE] = { "M", COLOR_MUSE },
        [MUSE_WEATHER_FROM_IP] = { "I", COLOR_TAG },
        [MUSE_WEATHER_FROM_DEFAULT] = { "F", COLOR_DIM },
    };
    if (strcmp(lv_label_get_text(s_source), SOURCES[w.from].mark) != 0) {
        lv_label_set_text(s_source, SOURCES[w.from].mark);
        lv_obj_set_style_bg_color(s_source, lv_color_hex(SOURCES[w.from].color), 0);
        lv_obj_remove_flag(s_source, LV_OBJ_FLAG_HIDDEN);
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "%ld" UNIT_MARK, lroundf(w.temp));
    set_text(s_temp, buf);
    if (w.humidity >= 0) {
        snprintf(buf, sizeof(buf), "%d%%", w.humidity);
        set_text(s_humidity, buf);
    }
    if (!isnan(w.high) && !isnan(w.low)) {
        snprintf(buf, sizeof(buf), "%ld°/%ld°", lroundf(w.high), lroundf(w.low));
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
        set_text(s_week, WEEK[tm.tm_wday]);
        strftime(buf, sizeof(buf), "%Y/%m/%d", &tm);
        set_text(s_date, buf);
    }
    update_status();
#if CONFIG_MUSE_WEATHER
    update_weather();
#endif
}

void muse_clock_view_activity(muse_mode_t mode, const char *name, float level)
{
    if (!s_root) {
        return;
    }
    uint32_t accent = muse_pixel_accent(mode);
    if (name != s_shown_name) {
        const char *shown = muse_lang_status(name);
        lv_obj_set_style_text_font(s_state, muse_lang_label_font(shown, &lv_font_montserrat_14), 0);
        lv_label_set_text(s_state, shown);
        s_shown_name = name;
    }
    if ((int)mode != s_shown_mode) {
        s_shown_mode = (int)mode;
        lv_obj_set_style_text_color(s_state, lv_color_hex(accent), 0);
        lv_obj_set_style_bg_color(s_level_fill, lv_color_hex(accent), 0);
    }

    bool heard = mode == MUSE_MODE_LISTENING || mode == MUSE_MODE_SPEAKING;
    int lit = heard ? LV_CLAMP(0, (int)lroundf(level * LEVEL_W), LEVEL_W) : -1;
    if (lit != s_shown_level) {
        lv_obj_set_style_bg_opa(s_level, lit < 0 ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
        lv_obj_set_width(s_level_fill, LV_MAX(lit, 0));
        s_shown_level = lit;
    }

    muse_state_caption(s_caption_text, sizeof(s_caption_text), &s_caption_version);
    bool reply = mode == MUSE_MODE_THINKING || mode == MUSE_MODE_SPEAKING;
    const char *caption = CONFIG_MUSE_HIDE_REPLY_CAPTIONS && reply ? "" : s_caption_text;
    if (strcmp(lv_label_get_text(s_caption), caption) != 0) {
        lv_obj_set_style_text_font(s_caption, muse_lang_label_font(caption, FONT_SMALL), 0);
        lv_label_set_text(s_caption, caption);
    }
}
