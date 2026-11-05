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

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MUSE_PLAY_BUFFER_S 10

typedef void (*muse_play_done_cb)(bool ok, const char *message, void *arg);

bool muse_play_sound_known(const char *name);
bool muse_play_busy(void);
bool muse_play_mp3(uint8_t *mp3, size_t len, muse_play_done_cb done, void *arg);
bool muse_play_sound(const char *name, muse_play_done_cb done, void *arg);

#ifdef __cplusplus
}
#endif
