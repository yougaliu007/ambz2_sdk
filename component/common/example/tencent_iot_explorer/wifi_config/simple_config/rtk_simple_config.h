/*
 * Copyright (c) 2020 Tencent Cloud. All rights reserved.

 * Licensed under the MIT License (the "License"); you may not use this file except in
 * compliance with the License. You may obtain a copy of the License at
 * http://opensource.org/licenses/MIT

 * Unless required by applicable law or agreed to in writing, software distributed under the License is
 * distributed on an "AS IS" basis, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
 * either express or implied. See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#ifndef __RTK_SIMPLE_CONFIG_H__
#define __RTK_SIMPLE_CONFIG_H__

#include <stdbool.h>

#include "qiot_internal.h"

#define MAX_SIZE_OF_PIN_CODE 8

typedef struct {
    char custom_pin_code[MAX_SIZE_OF_PIN_CODE + 1]; /* simple config pin code */
} eSimpleConfigParams;

/**
 * @brief Start simple config
 *
 * @param params See eSimpleConfigParams
 *
 * @return 0 when success, or err code for failure
 */
int simple_config_start(void *params, WifiConfigEventCallBack event_cb);

/**
 * @brief Stop WiFi config and device binding process
 */
void simple_config_stop(void);

#endif  // __RTK_SIMPLE_CONFIG_H__
