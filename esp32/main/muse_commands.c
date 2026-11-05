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

#include "muse_commands.h"

#include <string.h>

#include "esp_timer.h"
#include "sdkconfig.h"

#include "muse_input.h"
#include "muse_settings.h"
#include "muse_state.h"
#include "muse_ui.h"
#include "muse_voice.h"
#include "muse_wifi.h"
#if CONFIG_MUSE_WEATHER
#include "muse_weather.h"
#endif

static cJSON *typed(const char *type, const char *description)
{
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "type", type);
    cJSON_AddStringToObject(p, "description", description);
    return p;
}

static void add(cJSON *commands, const char *name, const char *description, cJSON *optional)
{
    cJSON *c = cJSON_CreateObject();
    cJSON_AddStringToObject(c, "description", description);
    cJSON_AddItemToObject(c, "required", cJSON_CreateObject());
    cJSON_AddItemToObject(c, "optional", optional ? optional : cJSON_CreateObject());
    cJSON_AddItemToObject(commands, name, c);
}

void muse_commands_register(cJSON *commands)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddItemToObject(o, "volume", typed("integer", "Speaker volume, 0-100."));
    cJSON_AddItemToObject(o, "speaker", typed("boolean", "Speaker on; off shows replies without playing them."));
    cJSON_AddItemToObject(o, "mic_gain", typed("integer", "Mic gain in dB, 0-36: about 20 low, 30 normal, 36 high."));
    cJSON_AddItemToObject(o, "brightness", typed("integer", "Screen brightness, 10-100."));
#if CONFIG_MUSE_CLOCK_SCREEN
    cJSON_AddItemToObject(o, "screen", typed("string", "\"on\" (Muse), \"clock\" (time and weather) or \"off\"."));
#else
    cJSON_AddItemToObject(o, "screen", typed("string", "\"on\" or \"off\"."));
#endif
    add(commands, "device.settings",
        "Change this gadget's settings; each is optional and kept across restarts (screen excepted). "
        "Returns them all.",
        o);
#if CONFIG_MUSE_REALTIME_VOICE
    o = cJSON_CreateObject();
    cJSON_AddItemToObject(o, "end", typed("boolean", "true ends the current call."));
    add(commands, "device.call", "Start a live voice call with you on this gadget, or end it.", o);
#endif
    add(commands, "device.power_off",
        "Turn this gadget off. On battery it powers fully off until its power key is pressed; on USB "
        "power it sleeps until a key press. The payload says which.",
        NULL);
    add(commands, "device.restart",
        "Restart this gadget: it powers down its screen, audio and processor for a second and boots "
        "again, back online in about 20 seconds.",
        NULL);
}

bool muse_commands_handles(const char *command)
{
    return !strcmp(command, "device.settings") || !strcmp(command, "device.call") ||
           !strcmp(command, "device.power_off") || !strcmp(command, "device.restart");
}

static cJSON *error(const char *code, const char *message)
{
    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "ok", false);
    cJSON *e = cJSON_AddObjectToObject(result, "error");
    cJSON_AddStringToObject(e, "code", code);
    cJSON_AddStringToObject(e, "message", message);
    return result;
}

static cJSON *ok(cJSON **payload)
{
    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "ok", true);
    *payload = cJSON_AddObjectToObject(result, "payload");
    return result;
}

static bool int_param(const cJSON *params, const char *key, int lo, int hi, int *out, bool *bad)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(params, key);
    if (!v) {
        return false;
    }
    if (!cJSON_IsNumber(v) || v->valuedouble < lo || v->valuedouble > hi) {
        *bad = true;
        return false;
    }
    *out = (int)v->valuedouble;
    return true;
}

static const char *screen_now(void)
{
    if (muse_state_asleep()) {
        return "off";
    }
#if CONFIG_MUSE_CLOCK_SCREEN
    if (muse_ui_clock()) {
        return "clock";
    }
#endif
    return "on";
}

static cJSON *settings(const cJSON *params)
{
    bool bad = false;
    int volume, mic_gain, brightness;
    bool set_volume = int_param(params, "volume", 0, 100, &volume, &bad);
    bool set_gain = int_param(params, "mic_gain", 0, MUSE_MIC_GAIN_MAX, &mic_gain, &bad);
    bool set_brightness = int_param(params, "brightness", 10, 100, &brightness, &bad);
    if (bad) {
        return error("invalid_param", "volume is 0-100, mic_gain 0-36 and brightness 10-100");
    }
    const cJSON *speaker = cJSON_GetObjectItemCaseSensitive(params, "speaker");
    if (speaker && !cJSON_IsBool(speaker)) {
        return error("invalid_param", "speaker is true or false");
    }
    const cJSON *screen = cJSON_GetObjectItemCaseSensitive(params, "screen");
    int view = -1;
    if (screen) {
        const char *s = cJSON_GetStringValue(screen);
        view = !s                     ? -1
               : !strcmp(s, "off")    ? MUSE_SCREEN_OFF
               : !strcmp(s, "on")     ? MUSE_SCREEN_ON
#if CONFIG_MUSE_CLOCK_SCREEN
               : !strcmp(s, "clock") ? MUSE_SCREEN_CLOCK
#endif
                                      : -1;
        if (view < 0) {
            return error("invalid_param", "screen is on, off or clock");
        }
    }
    if (set_volume) {
        muse_settings_set_volume(volume);
    }
    if (speaker) {
        muse_settings_set_speaker_on(cJSON_IsTrue(speaker));
    }
    if (set_gain) {
        muse_settings_set_mic_gain(mic_gain);
    }
    if (set_brightness) {
        muse_settings_set_brightness(brightness);
    }
    if (view >= 0) {
        muse_input_request_screen((muse_screen_t)view);
    }
    cJSON *payload;
    cJSON *result = ok(&payload);
    cJSON_AddNumberToObject(payload, "volume", muse_settings_volume());
    cJSON_AddBoolToObject(payload, "speaker", muse_settings_speaker_on());
    cJSON_AddNumberToObject(payload, "mic_gain", muse_settings_mic_gain());
    cJSON_AddNumberToObject(payload, "brightness", muse_settings_brightness());
    cJSON_AddStringToObject(payload, "screen", screen ? cJSON_GetStringValue(screen) : screen_now());
    return result;
}

void muse_commands_health(cJSON *health)
{
    muse_wifi_status_t w;
    muse_wifi_status(&w);
    cJSON *net = cJSON_AddObjectToObject(health, "network");
    cJSON_AddBoolToObject(net, "connected", w.state == MUSE_WIFI_CONNECTED);
    cJSON_AddStringToObject(net, "ssid", w.ssid);
    if (w.ip[0]) {
        cJSON_AddStringToObject(net, "ip", w.ip);
    }
    if (w.state == MUSE_WIFI_CONNECTED) {
        cJSON_AddNumberToObject(net, "rssi", w.rssi);
    }
#if CONFIG_MUSE_WEATHER
    muse_weather_t now;
    if (!muse_weather_now(&now)) {
        cJSON_AddNullToObject(health, "location");
        return;
    }
    static const char *const SOURCES[] = {
        [MUSE_WEATHER_FROM_MUSE] = "muse",
        [MUSE_WEATHER_FROM_IP] = "ip",
        [MUSE_WEATHER_FROM_DEFAULT] = "default",
    };
    static const char *const HOW[] = {
        [MUSE_WEATHER_FROM_MUSE] = "Muse said where you are when the gadget asked; it asks every 12 hours.",
        [MUSE_WEATHER_FROM_IP] = "From the network's public IP address (ipinfo.io), as Muse couldn't tell.",
        [MUSE_WEATHER_FROM_DEFAULT] = "The built-in fallback, as neither Muse nor the IP address gave a place.",
    };
    cJSON *loc = cJSON_AddObjectToObject(health, "location");
    cJSON_AddStringToObject(loc, "place", now.place);
    cJSON_AddNumberToObject(loc, "latitude", now.lat);
    cJSON_AddNumberToObject(loc, "longitude", now.lon);
    cJSON_AddStringToObject(loc, "source", SOURCES[now.from]);
    cJSON_AddStringToObject(loc, "how", HOW[now.from]);
    cJSON_AddNumberToObject(loc, "age_s", (double)((esp_timer_get_time() - now.located_us) / 1000000));
    if (now.offset_known) {
        cJSON_AddNumberToObject(loc, "utc_offset_s", now.utc_offset_s);
    }
#endif
}

cJSON *muse_commands_handle(const char *command, const cJSON *params)
{
    if (!strcmp(command, "device.settings")) {
        return settings(params);
    }
    cJSON *payload;
    if (!strcmp(command, "device.call")) {
#if CONFIG_MUSE_REALTIME_VOICE
        const cJSON *end = cJSON_GetObjectItemCaseSensitive(params, "end");
        if (end && !cJSON_IsBool(end)) {
            return error("invalid_param", "end is true or false");
        }
        bool start = !cJSON_IsTrue(end);
        bool in_call = muse_voice_in_call();
        cJSON *result = ok(&payload);
        cJSON_AddStringToObject(payload, "action", start ? (in_call ? "already_in_call" : "start")
                                                         : (in_call ? "end" : "no_call"));
        muse_input_request_call(start);
        return result;
#else
        return error("unsupported", "live calls are off in this firmware");
#endif
    }
    if (!strcmp(command, "device.power_off")) {
        muse_power_t p = muse_state_power();
        cJSON *result = ok(&payload);
        cJSON_AddStringToObject(payload, "action", p.usb ? "sleep" : "power_off");
        cJSON_AddStringToObject(payload, "note",
                                p.usb ? "On USB power the gadget can't cut its own power: it sleeps until a key press."
                                      : "It turns fully off; its power key turns it back on.");
        muse_input_request_power_off();
        return result;
    }
    cJSON *result = ok(&payload);
    cJSON_AddStringToObject(payload, "action", "restart");
    cJSON_AddStringToObject(payload, "note", "It will be back online in about 20 seconds.");
    muse_input_request_restart();
    return result;
}
