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

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "muse_input.h"
#include "muse_play.h"
#include "muse_settings.h"
#include "muse_state.h"
#include "muse_ui.h"
#include "muse_voice.h"
#include "muse_wifi.h"
#if CONFIG_MUSE_WEATHER
#include "muse_weather.h"
#endif

static const char *TAG = "muse_commands";

#define PLAY_MAX_BYTES (2 * 1024 * 1024)
#define PLAY_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

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
#if CONFIG_MUSE_CLOCK_SCREEN && !CONFIG_MUSE_CLOCK_MAIN
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
    o = cJSON_CreateObject();
    cJSON_AddItemToObject(o, "url", typed("string", "http:// or https:// URL of an MP3 to play."));
    cJSON_AddItemToObject(o, "path", typed("string", "An MP3 file on this VM, relative, e.g. workspace/hello.mp3."));
    cJSON_AddItemToObject(o, "sound", typed("string", "A built-in sound: \"chime\", \"beep\" or \"alarm\"."));
    add(commands, "device.play",
        "Play one of url, path or sound on this gadget's speaker, at its volume, over anything else playing, "
        "a live call included; an MP3 up to 2 MB. Replies once it has played. Fails while the speaker is off. "
        "A live call that ends while it plays, without a key press, is started again.",
        o);
    cJSON_AddNumberToObject(cJSON_GetObjectItem(commands, "device.play"), "timeout_ms", 120000);
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
    return !strcmp(command, "device.settings") || !strcmp(command, "device.call") || !strcmp(command, "device.play") ||
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

static cJSON *ok_result(cJSON **payload)
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
#if CONFIG_MUSE_CLOCK_SCREEN && !CONFIG_MUSE_CLOCK_MAIN
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
    cJSON *result = ok_result(&payload);
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
        [MUSE_WEATHER_FROM_MUSE] =
            "Muse said where you are when the gadget asked. It asks every 12 hours, and again when the gadget "
            "joins another Wi-Fi network or is plugged in, at most every 30 minutes.",
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

typedef struct {
    noise_ctrl_session_generation_t gen;
    char request_id[64];
    char *source;
    bool vm;
    SemaphoreHandle_t lock;
    uint8_t *buf;
    size_t len, cap;
    int status;
    bool done, complete, too_large;
    uint32_t token;
    bool in_call;
    uint32_t presses;
} play_ctx_t;

#if CONFIG_MUSE_REALTIME_VOICE
static bool call_again(const play_ctx_t *c)
{
    if (!c->in_call || muse_input_presses() != c->presses) {
        return false;
    }
    for (int i = 0; i < 100 && muse_voice_in_call(); i++) {
        if (muse_voice_call_view() != MUSE_VOICE_VIEW_ENDING) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (muse_voice_in_call()) {
        return false;
    }
    ESP_LOGI(TAG, "play: the call ended while it played, calling again");
    muse_input_request_call(true);
    return true;
}
#endif

static void play_done(bool ok, const char *message, void *arg)
{
    play_ctx_t *c = arg;
#if CONFIG_MUSE_REALTIME_VOICE
    bool called = call_again(c);
#else
    bool called = false;
#endif
    cJSON *result;
    if (ok) {
        cJSON *payload;
        result = ok_result(&payload);
        cJSON_AddStringToObject(payload, "message", message);
        if (called) {
            cJSON_AddStringToObject(payload, "call", "the call ended while it played; the gadget called Muse again");
        }
    } else {
        result = error("play_failed", message);
    }
    noise_ctrl_send_command_result(c->gen, c->request_id, result);
    if (c->lock) {
        vSemaphoreDelete(c->lock);
    }
    free(c->source);
    free(c);
}

static uint8_t *fetch_url(const char *url, size_t *len, char *err, size_t err_cap)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 4096,
    };
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    if (!h) {
        strlcpy(err, "out of memory", err_cap);
        return NULL;
    }
    uint8_t *buf = NULL;
    size_t cap = 0, n = 0;
    int status = 0;
    for (int hop = 0; hop < 4; hop++) {
        if (esp_http_client_open(h, 0) != ESP_OK) {
            snprintf(err, err_cap, "can't reach the URL");
            goto fail;
        }
        int64_t total = esp_http_client_fetch_headers(h);
        status = esp_http_client_get_status_code(h);
        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            esp_http_client_set_redirection(h);
            esp_http_client_close(h);
            continue;
        }
        if (status != 200) {
            snprintf(err, err_cap, "the URL answered HTTP %d", status);
            goto fail;
        }
        if (total > PLAY_MAX_BYTES) {
            snprintf(err, err_cap, "larger than %d MB", PLAY_MAX_BYTES / (1024 * 1024));
            goto fail;
        }
        cap = total > 0 ? (size_t)total : 256 * 1024;
        buf = heap_caps_malloc(cap, PLAY_CAPS);
        if (!buf) {
            strlcpy(err, "out of memory", err_cap);
            goto fail;
        }
        for (;;) {
            if (n == cap) {
                if (cap >= PLAY_MAX_BYTES) {
                    snprintf(err, err_cap, "larger than %d MB", PLAY_MAX_BYTES / (1024 * 1024));
                    goto fail;
                }
                size_t grow = cap * 2 > PLAY_MAX_BYTES ? PLAY_MAX_BYTES : cap * 2;
                uint8_t *more = heap_caps_realloc(buf, grow, PLAY_CAPS);
                if (!more) {
                    strlcpy(err, "out of memory", err_cap);
                    goto fail;
                }
                buf = more;
                cap = grow;
            }
            int got = esp_http_client_read(h, (char *)buf + n, (int)(cap - n));
            if (got <= 0) {
                break;
            }
            n += got;
        }
        esp_http_client_close(h);
        esp_http_client_cleanup(h);
        *len = n;
        return buf;
    }
    snprintf(err, err_cap, "too many redirects");
fail:
    heap_caps_free(buf);
    esp_http_client_close(h);
    esp_http_client_cleanup(h);
    return NULL;
}

static void vm_frame(void *ctx, int status, const uint8_t *data, size_t len, bool end)
{
    play_ctx_t *c = ctx;
    xSemaphoreTake(c->lock, portMAX_DELAY);
    if (!c->done) {
        if (status) {
            c->status = status;
        }
        bool ok = true;
        if (len && (c->status == 200 || c->status == 206)) {
            if (len > c->cap - c->len) {
                c->too_large = true;
                ok = false;
            } else {
                memcpy(c->buf + c->len, data, len);
                c->len += len;
            }
        }
        c->complete = end && status >= 0 && ok;
        c->done = end || status < 0 || !ok;
    }
    xSemaphoreGive(c->lock);
}

static char *vm_path(const char *path)
{
    while (*path == '/') {
        path++;
    }
    static const char prefix[] = "/fs/raw/";
    char *out = malloc(sizeof(prefix) + strlen(path) * 3);
    if (!out) {
        return NULL;
    }
    char *p = stpcpy(out, prefix);
    for (const unsigned char *s = (const unsigned char *)path; *s; s++) {
        if (isalnum(*s) || strchr("-._~/", *s)) {
            *p++ = (char)*s;
        } else {
            p += sprintf(p, "%%%02X", *s);
        }
    }
    *p = '\0';
    return out;
}

static uint8_t *fetch_vm(play_ctx_t *c, size_t *len, char *err, size_t err_cap)
{
    char *path = vm_path(c->source);
    size_t chunk = noise_ctrl_req_max_body();
    size_t cap = 256 * 1024;
    uint8_t *file = heap_caps_malloc(cap, PLAY_CAPS);
    if (!path || !file) {
        free(path);
        heap_caps_free(file);
        strlcpy(err, "out of memory", err_cap);
        return NULL;
    }
    size_t n = 0;
    for (;;) {
        size_t want = chunk < PLAY_MAX_BYTES - n ? chunk : PLAY_MAX_BYTES - n;
        if (!want) {
            snprintf(err, err_cap, "larger than %d MB", PLAY_MAX_BYTES / (1024 * 1024));
            goto fail;
        }
        if (n + want > cap) {
            size_t grow = cap * 2 > PLAY_MAX_BYTES ? PLAY_MAX_BYTES : cap * 2;
            uint8_t *more = heap_caps_realloc(file, grow, PLAY_CAPS);
            if (!more) {
                strlcpy(err, "out of memory", err_cap);
                goto fail;
            }
            file = more;
            cap = grow;
        }
        char range[48];
        snprintf(range, sizeof(range), "bytes=%u-%u", (unsigned)n, (unsigned)(n + want - 1));
        const char *headers[] = { "Range", range, NULL };
        xSemaphoreTake(c->lock, portMAX_DELAY);
        c->buf = file + n;
        c->cap = want;
        c->len = 0;
        c->status = 0;
        c->done = c->complete = c->too_large = false;
        xSemaphoreGive(c->lock);
        int64_t id = noise_ctrl_req_open("GET", path, headers, true, vm_frame, c);
        int64_t deadline = esp_timer_get_time() + 20 * 1000000LL;
        bool done = false;
        while (id && !done && esp_timer_get_time() < deadline) {
            vTaskDelay(pdMS_TO_TICKS(20));
            xSemaphoreTake(c->lock, portMAX_DELAY);
            done = c->done;
            xSemaphoreGive(c->lock);
        }
        xSemaphoreTake(c->lock, portMAX_DELAY);
        bool complete = c->complete;
        int status = c->status;
        size_t got = c->len;
        c->done = true;
        xSemaphoreGive(c->lock);
        if (!id || !complete) {
            if (id) {
                noise_ctrl_req_cancel(id);
                vTaskDelay(pdMS_TO_TICKS(200));
            }
            strlcpy(err, id ? "the file didn't come through" : "not connected to Muse", err_cap);
            goto fail;
        }
        if (status == 416) {
            break;
        }
        if (status == 404) {
            strlcpy(err, "no such file on the VM", err_cap);
            goto fail;
        }
        if (status != 206 && !(status == 200 && n == 0)) {
            snprintf(err, err_cap, "the VM answered HTTP %d", status);
            goto fail;
        }
        n += got;
        if (status == 200 || got < want) {
            break;
        }
    }
    free(path);
    *len = n;
    return file;
fail:
    free(path);
    heap_caps_free(file);
    return NULL;
}

static void fetch_task(void *arg)
{
    play_ctx_t *c = arg;
    char err[64] = "";
    size_t len = 0;
    uint8_t *mp3 = c->vm ? fetch_vm(c, &len, err, sizeof(err)) : fetch_url(c->source, &len, err, sizeof(err));
    if (!mp3) {
        ESP_LOGW(TAG, "play: %s", err);
        play_done(false, err, c);
    } else if (!muse_play_mp3(mp3, len, play_done, c)) {
        play_done(false, "the speaker is busy", c);
    }
    vTaskDelete(NULL);
}

static cJSON *play(const cJSON *params, const char *request_id, noise_ctrl_session_generation_t gen)
{
    const char *url = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(params, "url"));
    const char *path = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(params, "path"));
    const char *sound = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(params, "sound"));
    if (!!(url && *url) + !!(path && *path) + !!(sound && *sound) != 1) {
        return error("invalid_param", "give one of url, path or sound");
    }
    if (url && strncmp(url, "http://", 7) && strncmp(url, "https://", 8)) {
        return error("invalid_param", "url is http:// or https://");
    }
    if (sound && !muse_play_sound_known(sound)) {
        return error("invalid_param", "sound is chime, beep or alarm");
    }
    if (!muse_settings_speaker_on()) {
        return error("speaker_off", "the speaker is off; device.settings speaker true turns it on");
    }
    if (muse_play_busy()) {
        return error("busy", "already playing");
    }
    play_ctx_t *c = calloc(1, sizeof(*c));
    if (!c) {
        return error("out_of_memory", "failed to allocate");
    }
    c->gen = gen;
    strlcpy(c->request_id, request_id, sizeof(c->request_id));
#if CONFIG_MUSE_REALTIME_VOICE
    c->in_call = muse_voice_in_call();
    c->presses = muse_input_presses();
#endif
    if (sound) {
        if (!muse_play_sound(sound, play_done, c)) {
            free(c);
            return error("busy", "already playing");
        }
    } else {
        c->vm = path != NULL;
        c->source = strdup(path ? path : url);
        c->lock = xSemaphoreCreateMutex();
        if (!c->source || !c->lock ||
            xTaskCreateWithCaps(fetch_task, "muse_fetch", 6 * 1024, c, 4, NULL, PLAY_CAPS) != pdPASS) {
            if (c->lock) {
                vSemaphoreDelete(c->lock);
            }
            free(c->source);
            free(c);
            return error("out_of_memory", "failed to start the download");
        }
    }
    cJSON *async = cJSON_CreateObject();
    cJSON_AddBoolToObject(async, "_async", true);
    return async;
}

cJSON *muse_commands_handle(const char *command, const cJSON *params, const char *request_id,
                            noise_ctrl_session_generation_t session_generation)
{
    if (!strcmp(command, "device.settings")) {
        return settings(params);
    }
    if (!strcmp(command, "device.play")) {
        return play(params, request_id, session_generation);
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
        cJSON *result = ok_result(&payload);
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
        cJSON *result = ok_result(&payload);
        cJSON_AddStringToObject(payload, "action", p.usb ? "sleep" : "power_off");
        cJSON_AddStringToObject(payload, "note",
                                p.usb ? "On USB power the gadget can't cut its own power: it sleeps until a key press."
                                      : "It turns fully off; its power key turns it back on.");
        muse_input_request_power_off();
        return result;
    }
    cJSON *result = ok_result(&payload);
    cJSON_AddStringToObject(payload, "action", "restart");
    cJSON_AddStringToObject(payload, "note", "It will be back online in about 20 seconds.");
    muse_input_request_restart();
    return result;
}
