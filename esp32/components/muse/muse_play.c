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

#include "muse_play.h"

#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "minimp3.h"

#include "muse_audio.h"
#include "muse_state.h"

static const char *TAG = "muse_play";

#define RATE MUSE_AUDIO_RATE
#define BUFFER_FRAMES (RATE * MUSE_PLAY_BUFFER_S)
#define START_FRAMES RATE
#define PIECE_FRAMES 4096
#define CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

typedef struct {
    uint8_t *mp3;
    size_t len;
    char sound[12];
    muse_play_done_cb done;
    void *arg;
} job_t;

typedef struct {
    StreamBufferHandle_t sb;
    bool started, stopped;
    size_t frames;
} out_t;

static atomic_bool s_busy;
static QueueHandle_t s_jobs;

static const char *const SOUNDS[] = { "chime", "beep", "alarm" };

bool muse_play_sound_known(const char *name)
{
    for (size_t i = 0; i < sizeof(SOUNDS) / sizeof(SOUNDS[0]); i++) {
        if (!strcmp(name, SOUNDS[i])) {
            return true;
        }
    }
    return false;
}

bool muse_play_busy(void)
{
    return atomic_load(&s_busy) || muse_audio_overlay_active();
}

static void begin(out_t *o)
{
    if (!o->started) {
        o->started = true;
        muse_audio_overlay(o->sb);
        muse_state_nudge();
    }
}

static bool emit(out_t *o, const int16_t *pcm, size_t frames)
{
    const uint8_t *p = (const uint8_t *)pcm;
    size_t bytes = frames * sizeof(int16_t);
    while (bytes) {
        if (o->started && !muse_audio_overlay_active()) {
            o->stopped = true;
            return false;
        }
        size_t sent = xStreamBufferSend(o->sb, p, bytes, pdMS_TO_TICKS(100));
        p += sent;
        bytes -= sent;
        if (xStreamBufferBytesAvailable(o->sb) >= START_FRAMES * sizeof(int16_t)) {
            begin(o);
        }
    }
    o->frames += frames;
    return true;
}

static size_t tone(int16_t *out, size_t at, size_t cap, float hz, float secs, float decay)
{
    size_t n = (size_t)(secs * RATE);
    size_t fade = RATE / 200;
    for (size_t i = 0; i < n && at + i < cap; i++) {
        float t = (float)i / RATE;
        float env = decay > 0 ? expf(-t * decay) : 1.0f;
        if (i < fade) {
            env *= (float)i / fade;
        } else if (n - i < fade) {
            env *= (float)(n - i) / fade;
        }
        float v = sinf(2 * (float)M_PI * hz * t) + 0.25f * sinf(4 * (float)M_PI * hz * t);
        out[at + i] = (int16_t)(v * env * 0.45f * 32767 / 1.25f);
    }
    return at + n < cap ? at + n : cap;
}

static size_t gap(int16_t *out, size_t at, size_t cap, float secs)
{
    size_t n = (size_t)(secs * RATE);
    if (at + n > cap) {
        n = cap - at;
    }
    memset(out + at, 0, n * sizeof(int16_t));
    return at + n;
}

static void synth(out_t *o, const char *name, int16_t *buf, size_t cap)
{
    size_t at = 0;
    if (!strcmp(name, "chime")) {
        at = tone(buf, at, cap, 880, 0.35f, 4);
        at = tone(buf, at, cap, 660, 0.8f, 3);
    } else if (!strcmp(name, "beep")) {
        for (int i = 0; i < 2; i++) {
            at = tone(buf, at, cap, 1000, 0.15f, 0);
            at = gap(buf, at, cap, 0.1f);
        }
    } else {
        for (int i = 0; i < 6; i++) {
            at = tone(buf, at, cap, 1000, 0.2f, 0);
            at = tone(buf, at, cap, 1300, 0.2f, 0);
            at = gap(buf, at, cap, 0.2f);
        }
    }
    emit(o, buf, at);
}

static bool decode(out_t *o, const uint8_t *mp3, size_t len, int16_t *piece)
{
    mp3dec_t *dec = heap_caps_malloc(sizeof(mp3dec_t), CAPS);
    int16_t *pcm = heap_caps_malloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t), CAPS);
    bool any = false;
    if (dec && pcm) {
        mp3dec_init(dec);
        double pos = 0;
        int16_t last = 0;
        size_t off = 0;
        bool ok = true;
        while (ok && off < len) {
            mp3dec_frame_info_t info;
            int samples = mp3dec_decode_frame(dec, mp3 + off, (int)(len - off), pcm, &info);
            if (!info.frame_bytes) {
                break;
            }
            off += info.frame_bytes;
            if (!samples) {
                continue;
            }
            any = true;
            if (info.channels == 2) {
                for (int k = 0; k < samples; k++) {
                    pcm[k] = (int16_t)((pcm[2 * k] + pcm[2 * k + 1]) / 2);
                }
            }
            double step = (double)info.hz / RATE;
            size_t n = 0;
            while (ok && pos < samples) {
                int i = (int)pos;
                double f = pos - i;
                int16_t a = i == 0 ? last : pcm[i - 1];
                piece[n++] = (int16_t)(a + (pcm[i] - a) * f);
                pos += step;
                if (n == PIECE_FRAMES) {
                    ok = emit(o, piece, n);
                    n = 0;
                }
            }
            if (ok && n) {
                ok = emit(o, piece, n);
            }
            pos -= samples;
            last = pcm[samples - 1];
        }
    }
    heap_caps_free(dec);
    heap_caps_free(pcm);
    return any;
}

static void play(job_t *job)
{
    char msg[80] = "";
    bool ok = false;
    out_t o = { 0 };
    o.sb = xStreamBufferCreateWithCaps(BUFFER_FRAMES * sizeof(int16_t), sizeof(int16_t), CAPS);
    size_t piece_frames = job->mp3 ? PIECE_FRAMES : RATE * 4;
    int16_t *piece = heap_caps_malloc(piece_frames * sizeof(int16_t), CAPS);
    if (!o.sb || !piece) {
        snprintf(msg, sizeof(msg), "out of memory (largest PSRAM block %u KB)",
                 (unsigned)(heap_caps_get_largest_free_block(CAPS) / 1024));
    } else if (job->mp3 && !decode(&o, job->mp3, job->len, piece)) {
        strlcpy(msg, "not an MP3 the gadget can decode", sizeof(msg));
    } else {
        if (!job->mp3) {
            synth(&o, job->sound, piece, piece_frames);
        }
        if (!o.stopped && o.frames) {
            begin(&o);
        }
        muse_audio_overlay_end();
        while (o.started && muse_audio_overlay_active()) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        ok = true;
        if (o.stopped) {
            strlcpy(msg, "stopped by a key press", sizeof(msg));
        } else {
            snprintf(msg, sizeof(msg), "played %.1f s", (double)o.frames / RATE);
        }
        ESP_LOGI(TAG, "%s", msg);
    }
    muse_audio_overlay_stop();
    heap_caps_free(piece);
    if (o.sb) {
        vStreamBufferDeleteWithCaps(o.sb);
    }
    heap_caps_free(job->mp3);
    if (job->done) {
        job->done(ok, msg, job->arg);
    }
}

static void play_task(void *arg)
{
    (void)arg;
    for (;;) {
        job_t *job;
        if (xQueueReceive(s_jobs, &job, portMAX_DELAY) == pdTRUE) {
            play(job);
            heap_caps_free(job);
            atomic_store(&s_busy, false);
        }
    }
}

static bool start(job_t *job)
{
    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_busy, &expected, true)) {
        heap_caps_free(job->mp3);
        heap_caps_free(job);
        return false;
    }
    if (!s_jobs) {
        s_jobs = xQueueCreate(1, sizeof(job_t *));
        if (!s_jobs ||
            xTaskCreatePinnedToCoreWithCaps(play_task, "muse_play", 32 * 1024, NULL, 4, NULL, 0, CAPS) != pdPASS) {
            ESP_LOGE(TAG, "start failed");
            heap_caps_free(job->mp3);
            heap_caps_free(job);
            atomic_store(&s_busy, false);
            return false;
        }
    }
    xQueueSend(s_jobs, &job, portMAX_DELAY);
    return true;
}

bool muse_play_mp3(uint8_t *mp3, size_t len, muse_play_done_cb done, void *arg)
{
    job_t *job = heap_caps_calloc(1, sizeof(job_t), CAPS);
    if (!job) {
        heap_caps_free(mp3);
        return false;
    }
    job->mp3 = mp3;
    job->len = len;
    job->done = done;
    job->arg = arg;
    return start(job);
}

bool muse_play_sound(const char *name, muse_play_done_cb done, void *arg)
{
    if (!muse_play_sound_known(name)) {
        return false;
    }
    job_t *job = heap_caps_calloc(1, sizeof(job_t), CAPS);
    if (!job) {
        return false;
    }
    strlcpy(job->sound, name, sizeof(job->sound));
    job->done = done;
    job->arg = arg;
    return start(job);
}
