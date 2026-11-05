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

#include "muse_weather.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "muse_chat.h"
#include "muse_state.h"
#include "muse_wifi.h"

static const char *TAG = "muse_weather";

#define WEATHER_PERIOD_US (30 * 60 * 1000000LL)
#define RETRY_US (2 * 60 * 1000000LL)
#define LOCATE_PERIOD_US (12 * 3600 * 1000000LL)
#define RELOCATE_GAP_US (30 * 60 * 1000000LL)
#define POLL_MS 5000
#define ASK_WAIT_MS (6 * 60 * 1000)
#define MAX_RESPONSE 4096
#define PLACE_NAME_MAX 32

#if CONFIG_MUSE_WEATHER_FAHRENHEIT
#define UNIT "fahrenheit"
#define UNIT_MARK "°F"
#else
#define UNIT "celsius"
#define UNIT_MARK "°C"
#endif

#define QUESTION                                                                                          \
    "My Muse Gadget asks this automatically, to show the weather where I am. Where am I right now? "    \
    "Use what you know about me, such as my location, time zone, calendar or recent messages. "         \
    "Reply with one line and nothing else: city|latitude|longitude, for example "                       \
    "San Jose|37.3382|-121.8863, or unknown if you can't tell."

typedef enum { SRC_NONE, SRC_MUSE, SRC_IP, SRC_DEFAULT } source_t;

typedef struct {
    char name[PLACE_NAME_MAX];
    double lat, lon;
} place_t;

typedef struct {
    char *buf;
    size_t len;
} resp_t;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static char s_line[96];
static muse_weather_t s_now;
static bool s_have_now;
static SemaphoreHandle_t s_answered;
static char s_reply[512];
static bool s_replied;
static volatile uint32_t s_ask_gen;
static int64_t s_located_us;

void muse_weather_line(char *out, size_t cap)
{
    portENTER_CRITICAL(&s_lock);
    strlcpy(out, s_line, cap);
    portEXIT_CRITICAL(&s_lock);
}

bool muse_weather_now(muse_weather_t *out)
{
    portENTER_CRITICAL(&s_lock);
    bool have = s_have_now;
    if (have) {
        *out = s_now;
    }
    portEXIT_CRITICAL(&s_lock);
    return have;
}

static void set_weather(const char *line, const muse_weather_t *now)
{
    portENTER_CRITICAL(&s_lock);
    strlcpy(s_line, line, sizeof(s_line));
    s_now = *now;
    s_have_now = true;
    portEXIT_CRITICAL(&s_lock);
}

static esp_err_t on_http(esp_http_client_event_t *evt)
{
    resp_t *r = evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && r->len + evt->data_len < MAX_RESPONSE) {
        memcpy(r->buf + r->len, evt->data, evt->data_len);
        r->len += evt->data_len;
        r->buf[r->len] = '\0';
    }
    return ESP_OK;
}

static cJSON *get_json(const char *url, const char *what)
{
    char *buf = heap_caps_malloc(MAX_RESPONSE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        return NULL;
    }
    buf[0] = '\0';
    resp_t r = { buf, 0 };
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 15000,
        .event_handler = on_http,
        .user_data = &r,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    cJSON *json = NULL;
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c) {
        esp_err_t err = esp_http_client_perform(c);
        int status = esp_http_client_get_status_code(c);
        esp_http_client_cleanup(c);
        if (err == ESP_OK && status == 200) {
            json = cJSON_Parse(buf);
        } else {
            ESP_LOGW(TAG, "%s: %s, status %d", what, esp_err_to_name(err), status);
        }
    }
    free(buf);
    return json;
}

static bool blank(const char *s)
{
    while (*s && strchr(" \t\r`*\"'.", *s)) {
        s++;
    }
    return !*s;
}

static bool parse_place(const char *text, place_t *out)
{
    while (text && *text) {
        const char *end = strchr(text, '\n');
        size_t n = end ? (size_t)(end - text) : strlen(text);
        char line[160];
        if (n < sizeof(line)) {
            memcpy(line, text, n);
            line[n] = '\0';
            char *name = line;
            while (*name && strchr(" \t`*\"'", *name)) {
                name++;
            }
            char *bar1 = strchr(name, '|');
            char *bar2 = bar1 ? strchr(bar1 + 1, '|') : NULL;
            if (bar2) {
                *bar1 = *bar2 = '\0';
                char *e1, *e2;
                double lat = strtod(bar1 + 1, &e1);
                double lon = strtod(bar2 + 1, &e2);
                size_t len = strlen(name);
                while (len && strchr(" \t", name[len - 1])) {
                    name[--len] = '\0';
                }
                if (len && e1 != bar1 + 1 && e2 != bar2 + 1 && blank(e1) && blank(e2) && fabs(lat) <= 90 &&
                    fabs(lon) <= 180) {
                    strlcpy(out->name, name, sizeof(out->name));
                    len = strlen(out->name);
                    if (len == sizeof(out->name) - 1) {
                        while (len && ((unsigned char)out->name[len - 1] & 0xC0) == 0x80) {
                            len--;
                        }
                        if (len && ((unsigned char)out->name[len - 1] & 0x80)) {
                            len--;
                        }
                        out->name[len] = '\0';
                    }
                    out->lat = lat;
                    out->lon = lon;
                    return true;
                }
            }
        }
        text = end ? end + 1 : NULL;
    }
    return false;
}

static void on_answer(const char *reply, void *arg)
{
    if ((uint32_t)(uintptr_t)arg != s_ask_gen) {
        return;
    }
    s_replied = reply != NULL;
    strlcpy(s_reply, reply ? reply : "", sizeof(s_reply));
    xSemaphoreGive(s_answered);
}

static int ask_muse(place_t *out)
{
    if (!muse_hatch_ready()) {
        return -1;
    }
    xSemaphoreTake(s_answered, 0);
    uint32_t gen = ++s_ask_gen;
    if (!muse_hatch_ask(QUESTION, on_answer, (void *)(uintptr_t)gen)) {
        return -1;
    }
    if (xSemaphoreTake(s_answered, pdMS_TO_TICKS(ASK_WAIT_MS)) != pdTRUE) {
        s_ask_gen++;
        return -1;
    }
    if (!s_replied) {
        return -1;
    }
    if (parse_place(s_reply, out)) {
        return 1;
    }
    ESP_LOGI(TAG, "Muse doesn't know the location");
    return 0;
}

static bool ip_place(place_t *out)
{
    cJSON *json = get_json("https://ipinfo.io/json", "ipinfo.io");
    const char *city = cJSON_GetStringValue(cJSON_GetObjectItem(json, "city"));
    const char *loc = cJSON_GetStringValue(cJSON_GetObjectItem(json, "loc"));
    bool ok = false;
    if (city && city[0] && loc) {
        char text[128];
        snprintf(text, sizeof(text), "%s|%s", city, loc);
        char *comma = strrchr(text, ',');
        if (comma) {
            *comma = '|';
            ok = parse_place(text, out);
        }
    }
    cJSON_Delete(json);
    return ok;
}

const char *muse_weather_condition(int code)
{
    if (code == 0) return "Clear";
    if (code <= 2) return "Partly cloudy";
    if (code == 3) return "Cloudy";
    if (code == 45 || code == 48) return "Fog";
    if (code >= 51 && code <= 57) return "Drizzle";
    if (code >= 61 && code <= 67) return "Rain";
    if (code >= 71 && code <= 77) return "Snow";
    if (code >= 80 && code <= 82) return "Showers";
    if (code == 85 || code == 86) return "Snow showers";
    if (code >= 95) return "Storm";
    return "";
}

static double first_number(cJSON *array)
{
    cJSON *v = cJSON_GetArrayItem(array, 0);
    return cJSON_IsNumber(v) ? v->valuedouble : NAN;
}

static bool fetch_weather(const place_t *p, source_t src)
{
    char url[320];
    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
             "&current=temperature_2m,relative_humidity_2m,weather_code,is_day"
             "&daily=temperature_2m_max,temperature_2m_min&forecast_days=1&timezone=auto"
             "&temperature_unit=" UNIT,
             p->lat, p->lon);
    cJSON *json = get_json(url, "open-meteo");
    cJSON *now = cJSON_GetObjectItem(json, "current");
    cJSON *temp = cJSON_GetObjectItem(now, "temperature_2m");
    cJSON *code = cJSON_GetObjectItem(now, "weather_code");
    bool ok = cJSON_IsNumber(temp) && cJSON_IsNumber(code);
    if (ok) {
        cJSON *humidity = cJSON_GetObjectItem(now, "relative_humidity_2m");
        cJSON *day = cJSON_GetObjectItem(now, "is_day");
        cJSON *daily = cJSON_GetObjectItem(json, "daily");
        cJSON *offset = cJSON_GetObjectItem(json, "utc_offset_seconds");
        muse_weather_t w = {
            .from = src == SRC_MUSE ? MUSE_WEATHER_FROM_MUSE : src == SRC_IP ? MUSE_WEATHER_FROM_IP : MUSE_WEATHER_FROM_DEFAULT,
            .temp = (float)temp->valuedouble,
            .high = (float)first_number(cJSON_GetObjectItem(daily, "temperature_2m_max")),
            .low = (float)first_number(cJSON_GetObjectItem(daily, "temperature_2m_min")),
            .humidity = cJSON_IsNumber(humidity) ? (int)lround(humidity->valuedouble) : -1,
            .code = code->valueint,
            .day = !cJSON_IsNumber(day) || day->valueint != 0,
            .offset_known = cJSON_IsNumber(offset),
            .utc_offset_s = cJSON_IsNumber(offset) ? offset->valueint : 0,
            .lat = p->lat,
            .lon = p->lon,
            .located_us = s_located_us,
        };
        strlcpy(w.place, p->name, sizeof(w.place));
        char line[sizeof(s_line)];
        snprintf(line, sizeof(line), "%s %ld" UNIT_MARK " %s%s", p->name, lround(temp->valuedouble),
                 muse_weather_condition(code->valueint), src == SRC_MUSE ? " (M)" : src == SRC_IP ? " (I)" : " (F)");
        set_weather(line, &w);
        ESP_LOGI(TAG, "%s", line);
    }
    cJSON_Delete(json);
    return ok;
}

static void weather_task(void *arg)
{
    (void)arg;
    place_t place = { 0 }, found;
    source_t src = SRC_NONE;
    int64_t next_muse = 0, next_ip = 0, next_weather = 0, checked = 0;
    char ssid[MUSE_SSID_MAX + 1] = "";
    bool usb = false, moved = false;
    for (;;) {
        muse_wifi_status_t w;
        muse_wifi_status(&w);
        bool plugged = muse_state_power().usb;
        if (src != SRC_NONE && plugged && !usb && !moved) {
            ESP_LOGI(TAG, "plugged in: the location is checked again");
            moved = true;
        }
        usb = plugged;
        if (w.state != MUSE_WIFI_CONNECTED) {
            vTaskDelay(pdMS_TO_TICKS(POLL_MS));
            continue;
        }
        if (src != SRC_NONE && ssid[0] && strcmp(ssid, w.ssid) != 0 && !moved) {
            ESP_LOGI(TAG, "on another Wi-Fi network: the location is checked again");
            moved = true;
        }
        strlcpy(ssid, w.ssid, sizeof(ssid));
        int64_t now = esp_timer_get_time();
        bool relocate = moved && now - checked >= RELOCATE_GAP_US;
        if (relocate) {
            moved = false;
            next_muse = next_ip = now;
        }
        bool located = false;
        if (now >= next_muse) {
            checked = now;
            int asked = ask_muse(&found);
            if (asked > 0) {
                place = found;
                src = SRC_MUSE;
                s_located_us = now;
                located = true;
            }
            next_muse = now + (asked < 0 ? WEATHER_PERIOD_US : LOCATE_PERIOD_US);
        }
        if (!located && (src != SRC_MUSE || relocate) && now >= next_ip) {
            checked = now;
            if (ip_place(&found)) {
                place = found;
                src = SRC_IP;
                next_ip = now + LOCATE_PERIOD_US;
                s_located_us = now;
                located = true;
            } else {
                if (src == SRC_NONE && parse_place(CONFIG_MUSE_WEATHER_FALLBACK, &found)) {
                    place = found;
                    src = SRC_DEFAULT;
                    s_located_us = now;
                    located = true;
                }
                next_ip = now + (src == SRC_NONE ? RETRY_US : WEATHER_PERIOD_US);
            }
        }
        if (src != SRC_NONE && (located || now >= next_weather)) {
            next_weather = now + (fetch_weather(&place, src) ? WEATHER_PERIOD_US : RETRY_US);
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

void muse_weather_start(void)
{
    s_answered = xSemaphoreCreateBinary();
    if (!s_answered ||
        xTaskCreateWithCaps(weather_task, "muse_weather", 10 * 1024, NULL, 3, NULL,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "start failed");
    }
}
