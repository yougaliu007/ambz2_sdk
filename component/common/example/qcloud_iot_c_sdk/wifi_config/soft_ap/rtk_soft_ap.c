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

#include "rtk_soft_ap.h"

#include <sockets.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "FreeRTOS.h"
#include "freertos_service.h"
#include "lwip_netconf.h"
#include "main.h"
#include "osdep_service.h"
#include "platform_stdlib.h"
#include "qcloud_iot_export_log.h"
#include "qiot_internal.h"
#include "task.h"
#include "udp.h"
#include "wifi_conf.h"
#include "wifi_simple_config.h"
#include "wifi_simple_config_parser.h"
#include "wifi_util.h"


#define JOIN_SIMPLE_CONFIG   (uint32_t)(1 << 8)
#define GET_CHANNEL_INTERVAL 105
#define LEAVE_ACK_EARLY      1
#define SSID_SOFTAP_TIMEOUT  (30000)

#if LWIP_VERSION_MAJOR >= 2
#undef lwip_ntohl
#define lwip_ntohl lwip_htonl
#endif

#ifdef PACK_STRUCT_USE_INCLUDES
#include "arch/bpstruct.h"
#endif

#ifdef PACK_STRUCT_USE_INCLUDES
#include "arch/epstruct.h"
#endif

static int sg_soft_ap_terminate = 0;
static uint8_t sg_soft_ap_done = WIFI_CONFIG_ING;

extern int error_flag;
extern struct netif xnetif[NET_IF_NUM];
extern void dhcps_deinit(void);
extern void dhcps_init(struct netif * pnetif);

void set_soft_ap_config_result(eWiFiConfigState result)
{
    sg_soft_ap_done = result;
}

eWiFiConfigState get_soft_ap_config_result(void)
{
    return sg_soft_ap_done;
}

int start_softAP(const char *ssid, const char *psw, uint8_t ch)
{

    Log_i("start softAP entry ");

#if CONFIG_LWIP_LAYER
    struct ip_addr ipaddr;
    struct ip_addr netmask;
    struct ip_addr gw;
    struct netif * pnetif = &xnetif[0];

    dhcps_deinit();
#if LWIP_VERSION_MAJOR >= 2
    IP4_ADDR(ip_2_ip4(&ipaddr), 192, 168, 4, 1);
    IP4_ADDR(ip_2_ip4(&netmask), 255, 255, 255, 0);
    IP4_ADDR(ip_2_ip4(&gw), 192, 168, 4, 1);
    netif_set_addr(pnetif, ip_2_ip4(&ipaddr), ip_2_ip4(&netmask),ip_2_ip4(&gw));
#else
    IP4_ADDR(&ipaddr, 192, 168, 4, 1);
    IP4_ADDR(&netmask, 255, 255, 255, 0);
    IP4_ADDR(&gw, 192, 168, 4, 1);

    netif_set_addr(pnetif, &ipaddr, &netmask,&gw);
#endif
#endif

    Log_d("Disable Wi-Fi");
    wifi_off();
    vTaskDelay(20);

    Log_d("Enable Wi-Fi with AP mode");
    if(wifi_on(RTW_MODE_AP) < 0) {
        Log_d("wifi_on failed");
        return -1;
    }

    Log_d("Start AP");
    rtw_security_t security = (psw == NULL) ? RTW_SECURITY_OPEN : RTW_SECURITY_WPA2_AES_PSK;
    int psw_len  = (psw == NULL) ? 0 :strlen(psw);
    if(wifi_start_ap(ssid, security, (char *)psw, strlen(ssid), psw_len, ch) < 0) {
        Log_d("wifi_start_ap failed");
        return -1;
    }

    Log_d("Check AP running");
    int timeout = 20;
    while(1) {
        char essid[33];
        if(wext_get_ssid(WLAN0_NAME, (unsigned char *) essid) > 0) {
            if(strcmp((const char *) essid, (const char *)ssid) == 0) {
                Log_d(" %s started", ssid);
                break;
            }
        }
        if(timeout == 0) {
            Log_d("ERROR: Start AP timeout");
            return -1;
        }
        vTaskDelay(1 * configTICK_RATE_HZ);
        timeout --;
    }

    Log_d("Start DHCP server");
    // For more setting about DHCP, please reference fATWA in atcmd_wifi.c.
#if CONFIG_LWIP_LAYER
    dhcps_init(&xnetif[0]);
#endif

    return 0;
}

int stop_softAP(void)
{
    int ret = 0;
    wifi_off();
    if(wifi_on(RTW_MODE_STA) < 0) {
        Log_e(" wifi_on failed");
        ret = -1;
    }

    return ret;
}

static void _soft_ap_config_task(void *para)
{
    uint32_t time_count = WIFI_CONFIG_WAIT_TIME_MS / SOFT_AP_BLINK_TIME_MS;
    WifiConfigEventCallBack wifi_config_event_cb = para;

    while (!sg_soft_ap_terminate && (--time_count)) {
        eWiFiConfigState state = get_soft_ap_config_result();
        if(WIFI_CONFIG_ING == state) {
            HAL_SleepMs(SOFT_AP_BLINK_TIME_MS);
            continue;
        } else if(WIFI_CONFIG_FAIL == state) {
            wifi_config_event_cb(EVENT_WIFI_CONFIG_FAILED, NULL);
            goto exit;
        } else {
            wifi_config_event_cb(EVENT_WIFI_CONFIG_SUCCESS, NULL);
            goto exit;
        }
    }
    wifi_config_event_cb(EVENT_WIFI_CONFIG_TIMEOUT, NULL);


exit:
    vTaskDelete(NULL);
    return;
}

static int _soft_ap_task_start(void *params)
{
    if (xTaskCreate(_soft_ap_config_task, ((const char *)"_soft_ap_config_task"), 1024, params,
                    tskIDLE_PRIORITY + 5, NULL) != pdPASS) {
        Log_e("xTaskCreate(_soft_ap_config_task) failed!");
        return -1;
    }

    return 0;
}

/**
 * @brief Start simple config
 *
 * @param params See eSimpleConfigParams
 *
 * @return 0 when success, or err code for failure
 */
int soft_ap_provision_start(void *params, WifiConfigEventCallBack event_cb)
{
    int ret = 0;
    eSoftApConfigParams * apConfig =  (eSoftApConfigParams *)params;

    set_soft_ap_config_result(WIFI_CONFIG_ING);
    // start soft ap
    ret = start_softAP(apConfig->ssid, apConfig->psw, apConfig->ch);
    if (0 == ret) {
        Log_d("start softAP success!");
        ret = _soft_ap_task_start(event_cb);
        if (ret) {
            soft_ap_provision_stop();
        }
    }

    return ret;
}

/**
 * @brief Stop WiFi config and device binding process
 */
void soft_ap_provision_stop(void)
{
    sg_soft_ap_terminate = 1;
    stop_softAP();
}

int wifi_sta_connect(const char *ssid, const char *psw, uint8_t channel)
{
#define RETRY_TIMES     3
    int      ret   = -1;
    uint16_t retry = 0;

    wifi_off();
	HAL_SleepMs(20);
    if(wifi_on(RTW_MODE_STA) < 0) {
        Log_e(" wifi_on failed");
        return -1;
    }

    if (channel == 0) {
        Log_d("Designated Channel ID is 0, won't change");
    } else {
        Log_d("Set Channel to %d", channel);
        wifi_set_channel(channel);
    }

    while (ret == -1 && retry++ < RETRY_TIMES) {
        Log_d("Connect to AP \"%s\" use STA mode..., try %d", ssid, retry);
        // todo: need a translation for security type from awws to RTW
        if (wifi_connect(ssid, RTW_SECURITY_WPA_AES_PSK, psw, strlen(ssid), strlen(psw), -1, NULL) != 0) {
            Log_e("wifi_connect failed");
            ret = -1;
        } else {
            Log_d("Connected to AP \"%s\" use STA mode...", ssid);

            LwIP_DHCP(0, DHCP_START);
            if (error_flag == RTW_NO_ERROR) {
                Log_d("DHCP success");
                ret = 0;
            } else {
                // DHCP fail, might caused by timeout or the AP did not enable DHCP server
                if (error_flag == RTW_DHCP_FAIL) {
                    Log_e("DHCP fail, might caused by timeout or the AP did not enable DHCP server");
                }
                ret = -1;
            }
        }
    }

    return ret;
#undef RETRY_TIMES
}


