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

#ifndef __RTK_SOFT_AP_CONFIG_H__
#define __RTK_SOFT_AP_CONFIG_H__

#include <stdbool.h>

#include "qiot_internal.h"

typedef struct {
   const char *ssid;
   const char *psw;
   uint8_t ch;
} eSoftApConfigParams;

/**
 * @brief Start softAP provision
 *
 * @param params See eSoftApConfigParams
 *
 * @return 0 when success, or err code for failure
 */
int  soft_ap_provision_start(void *params, WifiConfigEventCallBack event_cb);

/**
 * @brief Stop softAp WiFi provision and device binding process
 */
void soft_ap_provision_stop(void);


#endif  // __RTK_SOFT_AP_CONFIG_H__