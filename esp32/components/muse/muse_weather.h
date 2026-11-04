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

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { MUSE_WEATHER_FROM_MUSE, MUSE_WEATHER_FROM_IP, MUSE_WEATHER_FROM_DEFAULT } muse_weather_from_t;

typedef struct {
    char place[32];
    muse_weather_from_t from;
    float temp, high, low;
    int humidity;
    int code;
    bool day;
} muse_weather_t;

void muse_weather_start(void);
void muse_weather_line(char *out, size_t cap);
bool muse_weather_now(muse_weather_t *out);
const char *muse_weather_condition(int code);

#ifdef __cplusplus
}
#endif
