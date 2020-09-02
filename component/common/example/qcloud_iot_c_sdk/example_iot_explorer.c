/*
 * Tencent is pleased to support the open source community by making IoT Hub available.
 * Copyright (C) 2016 THL A29 Limited, a Tencent company. All rights reserved.

 * Licensed under the MIT License (the "License"); you may not use this file except in
 * compliance with the License. You may obtain a copy of the License at
 * http://opensource.org/licenses/MIT

 * Unless required by applicable law or agreed to in writing, software distributed under the License is
 * distributed on an "AS IS" basis, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
 * either express or implied. See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include "example_iot_explorer.h"

#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "FreeRTOS.h"
#include "platform_opts.h"
#include "qcloud_iot_export.h"
#include "qcloud_iot_export_log.h"
#include "qcloud_iot_import.h"
#include "qiot_wifi_config.h"
#include "rtk_simple_config.h"
#include "rtk_soft_ap.h"
#include "task.h"
#include "wifi_conf.h"

#define MQTT_SAMPLE             1
#define DATATEMPLATE_SAMPLE     2
#define LIGHT_SCENARY_SAMPLE    3
#define OTA_SAMPLE              4
#define GATEWAY_SAMPLE          5
#define DYN_REG_SAMPLE          6
#define RUN_SAMPLE_TYPE         LIGHT_SCENARY_SAMPLE

#if (MQTT_SAMPLE == RUN_SAMPLE_TYPE)
#define USR_APP_TASK_STACK_SIZE      (4096) //Byte
#elif (LIGHT_SCENARY_SAMPLE == RUN_SAMPLE_TYPE)
#define USR_APP_TASK_STACK_SIZE      (8196) //Byte
#else
#define USR_APP_TASK_STACK_SIZE      (10240) //Byte
#endif

static bool sg_wifi_config_done = false;

extern int mqtt_sample(bool loop_flag);
extern int light_data_template_sample(void);

static void set_wifi_config_result(bool result)
{
    sg_wifi_config_done = result;
}

static bool get_wifi_config_result(void)
{
    return sg_wifi_config_done;
}

uint16_t is_wifi_connected(void)
{
    return (uint16_t)((!wifi_is_up(RTW_STA_INTERFACE)||(get_wifi_config_result() != true) || wifi_is_connected_to_ap() != RTW_SUCCESS) ? 0 : 1);
}

static void _wifi_config_result_cb(eWiFiConfigResult event, void *usr_data)
{
    Log_d("entry...");
    qiot_wifi_config_stop();
    switch (event) {
        case RESULT_WIFI_CONFIG_SUCCESS:
            Log_i("WiFi is ready, to do Qcloud IoT demo");
            Log_d("timestamp now:%d", HAL_Timer_current_sec());
            break;

        case RESULT_WIFI_CONFIG_TIMEOUT:
            Log_e("wifi config timeout!");
        case RESULT_WIFI_CONFIG_FAILED:
            Log_e("wifi config failed!");
            qiot_wifi_config_send_log();
            break;

        default:
            break;
    }

    set_wifi_config_result(true);
}

static void qcloud_demo_task(void *arg)
{
    int ret;

    set_wifi_config_result(false);
    while (!wifi_is_up(RTW_STA_INTERFACE)) {
        HAL_SleepMs(1000);
    }
#if WIFI_PROV_SOFT_AP_ENABLE
    Log_d("start softAP wifi provision");
    eSoftApConfigParams apConf = {"RTK8720-SAP", "12345678", 6};
    ret = qiot_wifi_config_start(WIFI_CONFIG_TYPE_SOFT_AP, &apConf, _wifi_config_result_cb);
#elif WIFI_PROV_SIMPLE_CONFIG_ENABLE
    Log_d("start simple config wifi provision");
    ret = qiot_wifi_config_start(WIFI_CONFIG_TYPE_SIMPLE_CONFIG, NULL, _wifi_config_result_cb);
#else
    Log_e("not supported wifi provision method");
    ret = -1;
#endif
    if (ret) {
        Log_e("start wifi config failed: %d", ret);
        goto exit;
    }

    Timer timer;
    countdown(&timer, 150);
    while(!is_wifi_connected() && !expired(&timer)) {
        HAL_SleepMs(100);
    }

    if(expired(&timer)) {
        goto exit;
    }

    Log_d("wifi connected start sample");
    if(LIGHT_SCENARY_SAMPLE == RUN_SAMPLE_TYPE) {
        light_data_template_sample();
    } else if(OTA_SAMPLE == RUN_SAMPLE_TYPE) {
        Log_d("OTA sample to be ported");
    } else if(GATEWAY_SAMPLE == RUN_SAMPLE_TYPE) {
        Log_d("Gateway sample to be ported");
    } else if(DYN_REG_SAMPLE == RUN_SAMPLE_TYPE) {
        Log_d("Dynamic register sample to be ported");
    } else {
        mqtt_sample(true);
    }

exit:
    vTaskDelete(NULL);
}

void example_tencent_iot_explorer(void)
{
    IOT_Log_Set_Level(eLOG_DEBUG);
    xTaskCreate(qcloud_demo_task, "qcloud_demo_task", USR_APP_TASK_STACK_SIZE, NULL, 3, NULL);
}
