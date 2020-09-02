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

#ifndef __QCLOUD_IOT_WIFI_CONFIG_H__
#define __QCLOUD_IOT_WIFI_CONFIG_H__

#define WIFI_PROV_SOFT_AP_ENABLE    	1  ///< wifi provisioning method: device AP, need Wechat Applets
#define WIFI_PROV_SIMPLE_CONFIG_ENABLE  1  ///< wifi provisioning method: simple config, need Wechat Applets

typedef enum
{
    WIFI_CONFIG_TYPE_SOFT_AP       = 0, /* TODO(fancyxu@tencent.com): Soft ap support on RTK */
    WIFI_CONFIG_TYPE_SMART_CONFIG  = 1, /* TODO(fancyxu@tencent.com): Smart config support on RTK */
    WIFI_CONFIG_TYPE_AIRKISS       = 2, /* TODO(fancyxu@tencent.com): Airkiss support on RTK */
    WIFI_CONFIG_TYPE_SIMPLE_CONFIG = 3, /* Only simple config is supported now */
} eWiFiConfigType;

typedef enum
{
    RET_WIFI_CONFIG_START_SUCCESS    = 0,
    ERR_UNKNOWN_WIFI_CONFIG_TYPE     = -1,
    ERR_UNSUPPORTED_WIFI_CONFIG_TYPE = -2,
    ERR_WIFI_CONFIG_START_FAILED     = -3,
    ERR_COMM_SERVICE_START_FAILED    = -4
} eWiFiConfigErrCode;

typedef enum
{
    RESULT_WIFI_CONFIG_SUCCESS, /* WiFi config success */
    RESULT_WIFI_CONFIG_FAILED,  /* WiFi config failed */
    RESULT_WIFI_CONFIG_TIMEOUT  /* WiFi config timeout */
} eWiFiConfigResult;

typedef void (*WifiConfigResultCallBack)(eWiFiConfigResult result, void *usr_data);

/**
 * @brief Start WiFi config and device binding process
 *
 * @param type @see eWiFiConfigType, only WIFI_CONFIG_TYPE_SIMPLE_CONFIG is supported now
 * @param params @see rtk_simple_config.h
 * @param result_cb callback to get wifi config result
 *
 * @return @see eWiFiConfigErrCode
 */
int qiot_wifi_config_start(eWiFiConfigType type, void *params, WifiConfigResultCallBack result_cb);

/**
 * @brief Stop WiFi config and device binding process immediately
 */
void qiot_wifi_config_stop(void);

/**
 * @brief Send wifi config log to mini program
 */
void qiot_wifi_config_send_log(void);

#endif  //__QCLOUD_WIFI_CONFIG_H__
