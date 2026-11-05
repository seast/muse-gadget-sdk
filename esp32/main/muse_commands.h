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

#include "cJSON.h"
#include "noise_control.h"

#ifdef __cplusplus
extern "C" {
#endif

void muse_commands_register(cJSON *commands);
bool muse_commands_handles(const char *command);
cJSON *muse_commands_handle(const char *command, const cJSON *params, const char *request_id,
                            noise_ctrl_session_generation_t session_generation);
void muse_commands_health(cJSON *health);

#ifdef __cplusplus
}
#endif
