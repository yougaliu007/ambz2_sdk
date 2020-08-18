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

#include "rtk_simple_config.h"

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

extern uint32_t rtw_join_status;
extern void rtk_sc_deinit(void);
extern int promisc_get_fixed_channel(void *, uint8_t *, int *);
extern unsigned char is_promisc_enabled(void);
extern int get_sc_profile_fmt(void);
extern int get_sc_profile_info(void *fmt_info_t);
extern int get_sc_dsoc_info(void *dsoc_info_t);
extern int rtl_dsoc_parse(u8 *mac_addr, u8 *buf, void *userdata, unsigned int *len);
extern struct netif xnetif[NET_IF_NUM];

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
PACK_STRUCT_BEGIN
struct ack_msg {
    PACK_STRUCT_FIELD(u8_t flag);
    PACK_STRUCT_FIELD(u16_t length);
    PACK_STRUCT_FIELD(u8_t smac[6]);
    PACK_STRUCT_FIELD(u8_t status);
    PACK_STRUCT_FIELD(u16_t device_type);
    PACK_STRUCT_FIELD(u32_t device_ip);
    PACK_STRUCT_FIELD(u8_t device_name[64]);
} PACK_STRUCT_STRUCT;
PACK_STRUCT_END
#ifdef PACK_STRUCT_USE_INCLUDES
#include "arch/epstruct.h"
#endif

#pragma pack(1)
struct scan_with_ssid_result {
    u8 len; /* len of a memory area store ap info */
    u8 mac[ETH_ALEN];
    int rssi;
    u8 sec_mode;
    u8 password_id;
    u8 channel;
    // char ssid[65];
};

static _sema sg_simple_config_finish_sema;
static struct ack_msg sg_ack_content;
static struct rtk_test_sc sg_backup_sc_ctx;
static int sg_is_promisc_callback_unlock;
static int sg_simple_config_result;
static int sg_simple_config_terminate;
static int sg_fixed_channel_num;
static int sg_is_need_connect_to_AP;
static uint32_t sg_cmd_start_time;

static _sema sg_sc_dsoc_sema;
static uint8_t sg_mac_addr[6];
static uint8_t sg_ssid[33];
static int sg_ssid_len;
static int sg_ssid_hidden;

static const int sg_simple_config_promisc_channel_tbl[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13};

static void _filter_add_enable(void)
{
#define MASK_SIZE 3
    uint8_t mask[MASK_SIZE]    = {0xFF, 0xFF, 0xFF};
    uint8_t pattern[MASK_SIZE] = {0x01, 0x00, 0x5e};

    rtw_packet_filter_pattern_t packet_filter;
    rtw_packet_filter_rule_t rule;

    packet_filter.offset    = 0;
    packet_filter.mask_size = 3;
    packet_filter.mask      = mask;
    packet_filter.pattern   = pattern;

    rule = RTW_POSITIVE_MATCHING;

    wifi_init_packet_filter();
    wifi_add_packet_filter(1, &packet_filter, rule);
    wifi_enable_packet_filter(1);
#undef MASK_SIZE
}

static void _filter_remove(void)
{
    wifi_disable_packet_filter(1);
    wifi_remove_packet_filter(1);
}

static void _deinit_simple_config(void)
{
    rtk_sc_deinit();
    rtw_join_status = 0;  // clear simple config status
    rtw_free_sema(&sg_simple_config_finish_sema);
}

static int _init_simple_config(char *pin_code)
{
    if (rtw_join_status & JOIN_SIMPLE_CONFIG) {
        Log_e("Promisc mode is running!");
        return -1;
    }
    rtw_join_status |= JOIN_SIMPLE_CONFIG;

    rtw_init_sema(&sg_simple_config_finish_sema, 0);

    sg_fixed_channel_num          = 0;
    sg_simple_config_result       = 0;
    sg_cmd_start_time             = xTaskGetTickCount();
    sg_ssid_len                   = 0;
    sg_is_promisc_callback_unlock = 1;
    sg_ssid_hidden                = 0;

    memset(sg_ssid, 0, sizeof(sg_ssid));
    memset(sg_mac_addr, 0, sizeof(sg_mac_addr));
    memset(&sg_ack_content, 0, sizeof(sg_ack_content));
    memset(&sg_backup_sc_ctx, 0, sizeof(sg_backup_sc_ctx));

    struct simple_config_lib_config lib_config;
    lib_config.free_fn    = (simple_config_free_fn)rtw_mfree;
    lib_config.malloc_fn  = (simple_config_malloc_fn)rtw_malloc;
    lib_config.memcmp_fn  = (simple_config_memcmp_fn)memcmp;
    lib_config.memcpy_fn  = (simple_config_memcpy_fn)memcpy;
    lib_config.memset_fn  = (simple_config_memset_fn)memset;
    lib_config.printf_fn  = (simple_config_printf_fn)printf;
    lib_config.strcpy_fn  = (simple_config_strcpy_fn)strcpy;
    lib_config.strlen_fn  = (simple_config_strlen_fn)strlen;
    lib_config.zmalloc_fn = (simple_config_zmalloc_fn)rtw_zmalloc;
#if CONFIG_LWIP_LAYER
    lib_config.ntohl_fn = lwip_ntohl;
#else
    lib_config.ntohl_fn      = _ntohl;
#endif
    lib_config.is_promisc_callback_unlock = &sg_is_promisc_callback_unlock;

    // custom_pin_code can be null
    if (rtk_sc_init(pin_code, &lib_config) < 0) {
        Log_e("Rtk_sc_init fail!");
        rtw_join_status = 0;  // clear simple config status
        rtw_free_sema(&sg_simple_config_finish_sema);
        return -1;
    }

    return 0;
}

static void _simple_config_callback(unsigned char *buf, unsigned int len, void *userdata)
{
    taskENTER_CRITICAL();
    if (sg_is_promisc_callback_unlock) {
        unsigned char *da       = buf;
        unsigned char *sa       = buf + ETH_ALEN;
        sg_simple_config_result = rtk_start_parse_packet(da, sa, len, userdata, (void *)&sg_backup_sc_ctx);
    }
    taskEXIT_CRITICAL();
}

//============================  connect to AP function begin ===========================//
static void _filter1_add_enable(void)
{
#define MASK1_SIZE 12
#define MASK2_SIZE 18
    uint8_t mask1[MASK1_SIZE]    = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    uint8_t pattern[MASK1_SIZE]  = {0xFF,           0xFF,           0xFF,           0xFF,
                                    0xFF,           0xFF,           sg_mac_addr[0], sg_mac_addr[1],
                                    sg_mac_addr[2], sg_mac_addr[3], sg_mac_addr[4], sg_mac_addr[5]
                                   };
    uint8_t pattern2[MASK1_SIZE] = {sg_mac_addr[0], sg_mac_addr[1], sg_mac_addr[2], sg_mac_addr[3],
                                    sg_mac_addr[4], sg_mac_addr[5], sg_mac_addr[0], sg_mac_addr[1],
                                    sg_mac_addr[2], sg_mac_addr[3], sg_mac_addr[4], sg_mac_addr[5]
                                   };
    uint8_t mask2[MASK2_SIZE]    = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00,
                                    0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
                                   };
    uint8_t pattern3[MASK2_SIZE] = {sg_mac_addr[0], sg_mac_addr[1], sg_mac_addr[2], sg_mac_addr[3], sg_mac_addr[4],
                                    sg_mac_addr[5], 0x00,           0x00,           0x00,           0x00,
                                    0x00,           0x00,           sg_mac_addr[0], sg_mac_addr[1], sg_mac_addr[2],
                                    sg_mac_addr[3], sg_mac_addr[4], sg_mac_addr[5]
                                   };

    rtw_packet_filter_pattern_t packet_filter, packet_filter2, packet_filter3;
    rtw_packet_filter_rule_t rule;

    packet_filter.offset    = 4;
    packet_filter.mask_size = 12;
    packet_filter.mask      = mask1;
    packet_filter.pattern   = pattern;

    packet_filter2.offset    = 10;
    packet_filter2.mask_size = 12;
    packet_filter2.mask      = mask1;
    packet_filter2.pattern   = pattern2;

    packet_filter3.offset    = 4;
    packet_filter3.mask_size = 18;
    packet_filter3.mask      = mask2;
    packet_filter3.pattern   = pattern3;

    rule = RTW_POSITIVE_MATCHING;

    wifi_init_packet_filter();
    wifi_add_packet_filter(1, &packet_filter2, rule);
    wifi_enable_packet_filter(1);
    wifi_add_packet_filter(2, &packet_filter3, rule);
    wifi_enable_packet_filter(2);
    if (sg_ssid_hidden == 0) {
        wifi_add_packet_filter(3, &packet_filter, rule);
        wifi_enable_packet_filter(3);
    }
#undef MASK1_SIZE
#undef MASK2_SIZE
}

static void _remove1_filter(void)
{
    wifi_disable_packet_filter(1);
    wifi_remove_packet_filter(1);
    wifi_disable_packet_filter(2);
    wifi_remove_packet_filter(2);
    if (sg_ssid_hidden == 0) {
        wifi_disable_packet_filter(3);
        wifi_remove_packet_filter(3);
    }
}

static void _check_and_set_security_in_connection(rtw_security_t security_mode, rtw_network_info_t *wifi)
{
    if (security_mode == RTW_SECURITY_WPA2_AES_PSK) {
        printf("\r\nwifi->security_type = RTW_SECURITY_WPA2_AES_PSK\n");
        wifi->security_type = RTW_SECURITY_WPA2_AES_PSK;
    } else if (security_mode == RTW_SECURITY_WEP_PSK) {
        printf("\r\nwifi->security_type = RTW_SECURITY_WEP_PSK\n");
        wifi->security_type = RTW_SECURITY_WEP_PSK;
        wifi->key_id        = 0;
    } else if (security_mode == RTW_SECURITY_WPA_AES_PSK) {
        printf("\r\nwifi->security_type = RTW_SECURITY_WPA_AES_PSK\n");
        wifi->security_type = RTW_SECURITY_WPA_AES_PSK;
    } else {
        printf("\r\nwifi->security_type = RTW_SECURITY_OPEN\n");
        wifi->security_type = RTW_SECURITY_OPEN;
    }
}

static int _sc_set_val1(rtw_network_info_t *wifi, int fmt_val)
{
    int ret = -1;
    if (fmt_val == 1) {
        struct fmt_info *fmt_info_t = malloc(sizeof(struct fmt_info));
        memset(fmt_info_t, 0, sizeof(struct fmt_info));
        get_sc_profile_info(fmt_info_t);
        sg_fixed_channel_num = fmt_info_t->fmt_channel[1];
        sg_ssid_hidden       = fmt_info_t->fmt_hidden[0];

        rtw_memcpy(sg_mac_addr, fmt_info_t->fmt_bssid, 6);
        memset(sg_backup_sc_ctx.ssid, 0, sizeof(sg_backup_sc_ctx.ssid));

        if (memcmp(sg_mac_addr, g_bssid, 6) == 0) {
            rtw_memcpy(sg_backup_sc_ctx.ssid, sg_ssid, sg_ssid_len);
            wifi->ssid.len = strlen((char *)sg_backup_sc_ctx.ssid);
            rtw_memcpy(wifi->ssid.val, sg_backup_sc_ctx.ssid, wifi->ssid.len);
            wifi->ssid.val[wifi->ssid.len] = 0;
            printf("using ssid from profile and scan result\n");
        } else {
            rtw_memcpy(g_bssid, sg_mac_addr, 6);
        }

        free(fmt_info_t);
        ret = 0;
    }
    return ret;
}

static void sc_callback_handler(unsigned char *buf, unsigned int len, void *userdata)
{
    int ret = -1;
    taskENTER_CRITICAL();
    ret = rtl_dsoc_parse(sg_mac_addr, buf, userdata, &len);
    taskEXIT_CRITICAL();
    if (ret == 0) {
        // printf("\nhandler part\n");
        rtw_up_sema(&sg_sc_dsoc_sema);
        return;
    }
}

static int _sc_set_val2(rtw_network_info_t *wifi, int fmt_val)
{
    int ret = 1;
    struct dsoc_info *dsoc_info_t;

    if (fmt_val == 1) {
        _filter_remove();
        _filter1_add_enable();

        wifi_set_channel(sg_fixed_channel_num);
        rtw_init_sema(&sg_sc_dsoc_sema, 0);
        if (wifi_set_promisc(RTW_PROMISC_ENABLE_2, sc_callback_handler, 1) != 0) {
            printf("\nset promisc failed\n");
            rtw_free_sema(&sg_sc_dsoc_sema);
            ret = -1;
        }
        if (rtw_down_timeout_sema(&sg_sc_dsoc_sema, SSID_SOFTAP_TIMEOUT) == RTW_FALSE) {
            printf("\nsc callback failed\n");
            wifi_set_promisc(RTW_PROMISC_DISABLE, NULL, 0);
            rtw_free_sema(&sg_sc_dsoc_sema);
            ret = -1;
        } else {
            wifi_set_promisc(RTW_PROMISC_DISABLE, NULL, 0);
            dsoc_info_t = malloc(sizeof(struct dsoc_info));
            memset(dsoc_info_t, 0, sizeof(struct dsoc_info));
            get_sc_dsoc_info(dsoc_info_t);
            wifi->ssid.len = dsoc_info_t->dsoc_length;
            rtw_memcpy(wifi->ssid.val, dsoc_info_t->dsoc_ssid, wifi->ssid.len);
            wifi->ssid.val[wifi->ssid.len] = 0;
            rtw_free_sema(&sg_sc_dsoc_sema);
            free(dsoc_info_t);
            ret = 1;
        }
        _remove1_filter();
    }
    return ret;
}

static int _get_connection_info_from_profile(rtw_security_t security_mode, rtw_network_info_t *wifi, int fmt_val)
{
    printf("\r\n======= Connection Information =======\n");

    _check_and_set_security_in_connection(security_mode, wifi);

    wifi->password     = sg_backup_sc_ctx.password;
    wifi->password_len = (int)strlen((char const *)sg_backup_sc_ctx.password);

    /* 1.both scanned g_ssid and ssid from profile are null, return fail */
    if ((0 == sg_ssid_len) && (0 == strlen((char const *)sg_backup_sc_ctx.ssid))) {
        printf("no ssid info found, connect will fail\n");
        return -1;
    }

    if (_sc_set_val1(wifi, fmt_val) == 0) {
        goto ssid_set_done;
    }

    /* g_ssid and ssid from profile are same, enter connect and retry */
    if (0 == strcmp((char const *)sg_backup_sc_ctx.ssid, (char const *)sg_ssid)) {
        wifi->ssid.len = strlen((char const *)sg_backup_sc_ctx.ssid);
        rtw_memcpy(wifi->ssid.val, sg_backup_sc_ctx.ssid, wifi->ssid.len);
        wifi->ssid.val[wifi->ssid.len] = 0;
        printf("using ssid from profile and scan result\n");
        goto ssid_set_done;
    }

    /* if there is profile, but g_ssid and profile are different, using profile to connect and retry */
    if (strlen((char const *)sg_backup_sc_ctx.ssid) > 0) {
        wifi->ssid.len = strlen((char const *)sg_backup_sc_ctx.ssid);
        rtw_memcpy(wifi->ssid.val, sg_backup_sc_ctx.ssid, wifi->ssid.len);
        wifi->ssid.val[wifi->ssid.len] = 0;
        printf("using ssid only from profile\n");
        goto ssid_set_done;
    }

    /* if there is no profile but have scanned ssid, using g_ssid to connect and retry
            (maybe ssid is right and password is wrong) */
    if (sg_ssid_len > 0) {
        wifi->ssid.len = sg_ssid_len;
        rtw_memcpy(wifi->ssid.val, sg_ssid, wifi->ssid.len);
        wifi->ssid.val[wifi->ssid.len] = 0;
        printf("using ssid only from scan result\n");
        goto ssid_set_done;
    }

ssid_set_done:
    if (wifi->security_type == RTW_SECURITY_WEP_PSK) {
        if (wifi->password_len == 10) {
            u32 p[5] = {0};
            u8 pwd[6], i = 0;
            sscanf((const char *)sg_backup_sc_ctx.password, "%02x%02x%02x%02x%02x", &p[0], &p[1], &p[2], &p[3], &p[4]);
            for (i = 0; i < 5; i++) pwd[i] = (u8)p[i];
            pwd[5] = '\0';
            memset(sg_backup_sc_ctx.password, 0, 65);
            strcpy((char *)sg_backup_sc_ctx.password, (char *)pwd);
            wifi->password_len = 5;
        } else if (wifi->password_len == 26) {
            u32 p[13] = {0};
            u8 pwd[14], i = 0;
            sscanf((const char *)sg_backup_sc_ctx.password,
                   "%02x%02x%02x%02x%02x%02x%02x"
                   "%02x%02x%02x%02x%02x%02x",
                   &p[0], &p[1], &p[2], &p[3], &p[4], &p[5], &p[6], &p[7], &p[8], &p[9], &p[10], &p[11], &p[12]);
            for (i = 0; i < 13; i++) pwd[i] = (u8)p[i];
            pwd[13] = '\0';
            memset(sg_backup_sc_ctx.password, 0, 64);
            strcpy((char *)sg_backup_sc_ctx.password, (char *)pwd);
            wifi->password_len = 13;
        }
    }
    printf("\r\nwifi.password = %s\n", wifi->password);
    printf("\r\nwifi.password_len = %d\n", wifi->password_len);
    printf("\r\nwifi.ssid = %s\n", wifi->ssid.val);
    printf("\r\nwifi.ssid_len = %d\n", wifi->ssid.len);
    printf("\r\nwifi.channel = %d\n", sg_fixed_channel_num);
    printf("\r\n===== start to connect target AP =====\n");
    return 0;
}

static rtw_security_t _SC_translate_security(u8 security_type)
{
    rtw_security_t security_mode = RTW_SECURITY_UNKNOWN;

    switch (security_type) {
        case RTW_ENCRYPTION_OPEN:
            security_mode = RTW_SECURITY_OPEN;
            break;
        case RTW_ENCRYPTION_WEP40:
        case RTW_ENCRYPTION_WEP104:
            security_mode = RTW_SECURITY_WEP_PSK;
            break;
        case RTW_ENCRYPTION_WPA_TKIP:
        case RTW_ENCRYPTION_WPA_AES:
        case RTW_ENCRYPTION_WPA2_TKIP:
        case RTW_ENCRYPTION_WPA2_AES:
        case RTW_ENCRYPTION_WPA2_MIXED:
            security_mode = RTW_SECURITY_WPA2_AES_PSK;
            break;
        case RTW_ENCRYPTION_UNKNOWN:
        case RTW_ENCRYPTION_UNDEF:
        default:
            printf("unknow security mode,connect fail\n");
    }

    return security_mode;
}

static rtw_security_t _SC_translate_iw_security_mode(u8 security_type)
{
    rtw_security_t security_mode = RTW_SECURITY_UNKNOWN;

    switch (security_type) {
        case IW_ENCODE_ALG_NONE:
            security_mode = RTW_SECURITY_OPEN;
            break;
        case IW_ENCODE_ALG_WEP:
            security_mode = RTW_SECURITY_WEP_PSK;
            break;
        case IW_ENCODE_ALG_CCMP:
            security_mode = RTW_SECURITY_WPA2_AES_PSK;
            break;
        default:
            printf("error: security type not supported\n");
            break;
    };

    return security_mode;
}

/*
    scan buf format:
    len mac rssi sec    wps channel      ssid
    1B  6B  4B   1B     1B      1B      (len - 14)B
*/
static int _SC_parse_scan_result_and_connect(scan_buf_arg *scan_buf, rtw_network_info_t *wifi)
{
    struct scan_with_ssid_result scan_result;

    char *buf   = scan_buf->buf;
    int buf_len = scan_buf->buf_len;
    char ssid[65];
    int ssid_len         = 0;
    int parsed_len       = 0;
    uint8_t scan_channel = 0;
    int i                = 0;
    enum sc_result ret;
    uint8_t pscan_config = PSCAN_ENABLE | PSCAN_SIMPLE_CONFIG;

    memset((void *)&scan_result, 0, sizeof(struct scan_with_ssid_result));

    /* if wifi_is_connected_to_ap and we run here, ther will be hardfault(caused by auto reconnect) */
    printf("Scan result got, start to connect AP with scanned bssid\n");

    while (1) {
        memcpy(&scan_result, buf, sizeof(struct scan_with_ssid_result));
        /* len maybe 3*/
        if (scan_result.len < sizeof(struct scan_with_ssid_result)) {
            printf("length = %d, too small!\n", scan_result.len);
            goto sc_connect_wifi_fail;
        }

        /* set ssid */
        memset(ssid, 0, 65);

        ssid_len = scan_result.len - sizeof(struct scan_with_ssid_result);

        memcpy(ssid, buf + sizeof(struct scan_with_ssid_result), ssid_len);

        /* run here means there is a match */
        if (ssid_len == wifi->ssid.len) {
            if (memcmp(ssid, wifi->ssid.val, ssid_len) == 0) {
                printf("Connecting to  MAC=%02x:%02x:%02x:%02x:%02x:%02x, ssid = %s, SEC=%d\n", scan_result.mac[0],
                       scan_result.mac[1], scan_result.mac[2], scan_result.mac[3], scan_result.mac[4],
                       scan_result.mac[5], ssid, scan_result.sec_mode);

                scan_channel = scan_result.channel;

                // translate wep key if get_connection_info_from_profile() does not do it due to wrong security form
                // locked ssid for dual band router
                if (_SC_translate_iw_security_mode(scan_result.sec_mode) == RTW_SECURITY_WEP_PSK) {
                    if (wifi->password_len == 10) {
                        u32 p[5] = {0};
                        u8 pwd[6], i = 0;
                        sscanf((const char *)sg_backup_sc_ctx.password, "%02x%02x%02x%02x%02x", &p[0], &p[1], &p[2],
                               &p[3], &p[4]);
                        for (i = 0; i < 5; i++) pwd[i] = (u8)p[i];
                        pwd[5] = '\0';
                        memset(sg_backup_sc_ctx.password, 0, 65);
                        strcpy((char *)sg_backup_sc_ctx.password, (char *)pwd);
                        wifi->password_len = 5;
                    } else if (wifi->password_len == 26) {
                        u32 p[13] = {0};
                        u8 pwd[14], i = 0;
                        sscanf((const char *)sg_backup_sc_ctx.password,
                               "%02x%02x%02x%02x%02x%02x%02x"
                               "%02x%02x%02x%02x%02x%02x",
                               &p[0], &p[1], &p[2], &p[3], &p[4], &p[5], &p[6], &p[7], &p[8], &p[9], &p[10], &p[11],
                               &p[12]);
                        for (i = 0; i < 13; i++) pwd[i] = (u8)p[i];
                        pwd[13] = '\0';
                        memset(sg_backup_sc_ctx.password, 0, 64);
                        strcpy((char *)sg_backup_sc_ctx.password, (char *)pwd);
                        wifi->password_len = 13;
                    }
                }

                /* try 3 times to connect */
                for (i = 0; i < 3; i++) {
                    if (wifi_set_pscan_chan(&scan_channel, &pscan_config, 1) < 0) {
                        printf("\n\rERROR: wifi set partial scan channel fail");
                        ret = SC_TARGET_CHANNEL_SCAN_FAIL;
                        goto sc_connect_wifi_fail;
                    }

                    rtw_join_status = 0;

                    ret = (enum sc_result)wifi_connect_bssid(
                              scan_result.mac, (char *)wifi->ssid.val, _SC_translate_iw_security_mode(scan_result.sec_mode),
                              (char *)wifi->password, ETH_ALEN, wifi->ssid.len, wifi->password_len, 0, NULL);

                    if (ret == (enum sc_result)RTW_SUCCESS)
                        goto sc_connect_wifi_success;
                }
            }
        }

        buf = buf + scan_result.len;
        parsed_len += scan_result.len;
        if (parsed_len >= buf_len) {
            printf("parsed=%d, total = %d\n", parsed_len, buf_len);
            break;
        }
    }

sc_connect_wifi_success:
    printf("%s success\n", __FUNCTION__);
    return ret;

sc_connect_wifi_fail:
    printf("%s fail\n", __FUNCTION__);
    return ret;
}

/*
    When BSSID_CHECK_SUPPORT is not set, there will be problems:

    1.AP1 and AP2 (different SSID) both forward simple config packets,
    profile is from AP2, but Ameba connect with AP1
    2.AP1 and AP2 (same SSID, but different crypto or password), both forward simple config packets,
    profile is from AP2, but Ameba connect with AP1
    3. ...

    fix: using SSID to query matched BSSID(s) in scan result, traverse and connect.

    Consideration:
    1.Only take ssid and password
    2.Assume they have different channel.
    3.Assume they have different encrypt methods
*/
static int _SC_connect_to_candidate_AP(rtw_network_info_t *wifi)
{
    int ret;

    scan_buf_arg scan_buf;
    volatile int scan_cnt = 0;
    char *ssid            = (char *)wifi->ssid.val;
    int ssid_len          = wifi->ssid.len;

    printf("\nConnect with SSID=%s  password=%s\n", wifi->ssid.val, wifi->password);

    /* scan buf init */
    scan_buf.buf_len = 1000;
    scan_buf.buf     = (char *)pvPortMalloc(scan_buf.buf_len);
    if (!scan_buf.buf) {
        printf("\n\rERROR: Can't malloc memory");
        return RTW_NOMEM;
    }

    /* set ssid_len, ssid to scan buf */
    memset(scan_buf.buf, 0, scan_buf.buf_len);
    if (ssid && ssid_len > 0 && ssid_len <= 32) {
        memcpy(scan_buf.buf, &ssid_len, sizeof(int));
        memcpy(scan_buf.buf + sizeof(int), ssid, ssid_len);
    }

    /* call wifi scan to scan */
    if ((scan_cnt = (wifi_scan(RTW_SCAN_TYPE_ACTIVE, RTW_BSS_TYPE_ANY, &scan_buf))) < 0) {
        printf("\n\rERROR: wifi scan failed");
        ret = RTW_ERROR;
    } else {
        ret = _SC_parse_scan_result_and_connect(&scan_buf, wifi);
    }

    if (scan_buf.buf)
        vPortFree(scan_buf.buf);

    return ret;
}

static int _SC_check_and_show_connection_info(void)
{
    rtw_wifi_setting_t setting;

#if CONFIG_LWIP_LAYER
    int ret   = -1;
    int retry = 0;

    vTaskPrioritySet(NULL, tskIDLE_PRIORITY + 3);
    while (retry < 2) {
        /* If not rise priority, LwIP DHCP may timeout */
        /* Start DHCP Client */
        ret = LwIP_DHCP(0, DHCP_START);
        if (ret == DHCP_ADDRESS_ASSIGNED)
            break;
        else
            retry++;
    }
    vTaskPrioritySet(NULL, tskIDLE_PRIORITY + 1);
#endif

#if !(defined(CONFIG_EXAMPLE_UART_ATCMD) && CONFIG_EXAMPLE_UART_ATCMD) || \
    (defined(CONFIG_EXAMPLE_SPI_ATCMD) && CONFIG_EXAMPLE_SPI_ATCMD)
    wifi_get_setting(WLAN0_NAME, &setting);
    wifi_show_setting(WLAN0_NAME, &setting);
#endif

#if CONFIG_LWIP_LAYER
    if (ret != DHCP_ADDRESS_ASSIGNED)
        return SC_DHCP_FAIL;
    else
#endif
        return SC_SUCCESS;
}

static int _SC_connect_to_AP(void)
{
    int ret = SC_ERROR;

    uint8_t scan_channel = 0;
    uint8_t pscan_config;
    int fmt_val;
    int max_retry = 5, retry = 0;
    rtw_security_t security_mode;
    rtw_network_info_t wifi = {0};

    if (sg_fixed_channel_num) {
        scan_channel = sg_fixed_channel_num;
    }

    pscan_config = PSCAN_ENABLE | PSCAN_SIMPLE_CONFIG;

    security_mode   = _SC_translate_security(g_security_mode);
    g_security_mode = 0xff;  // clear it
    fmt_val         = get_sc_profile_fmt();

    if (-1 == _get_connection_info_from_profile(security_mode, &wifi, fmt_val)) {
        ret = SC_CONTROLLER_INFO_PARSE_FAIL;
        goto wifi_connect_fail;
    }

#if CONFIG_AUTO_RECONNECT
    /* disable auto reconnect */
    wifi_set_autoreconnect(0);
#endif

    /* optimization: get g_bssid to connect with only pscan */
    while (1) {
        ret = _sc_set_val2(&wifi, fmt_val);
        if (ret == 1) {
            // for dual band router, locked channel may not be target channel
            if (wifi_set_pscan_chan(&scan_channel, &pscan_config, 1) < 0) {
                printf("\n\rERROR: wifi set partial scan channel fail");
                ret = SC_TARGET_CHANNEL_SCAN_FAIL;
                goto wifi_connect_fail;
            }

            rtw_join_status = 0;

            ret = wifi_connect_bssid(g_bssid, (char *)wifi.ssid.val, wifi.security_type, (char *)wifi.password,
                                     ETH_ALEN, wifi.ssid.len, wifi.password_len, wifi.key_id, NULL);
        } else {
            goto wifi_connect_fail;
        }

        if (ret == RTW_SUCCESS) {
            goto wifi_connect_success;
        }

        if (retry == max_retry) {
            printf("connect fail with bssid, try ssid instead\n");
            break;
        }
        retry++;
    }

    /* when optimization fail: if connect with bssid fail because of we have connect to the wrong AP */
    ret = _SC_connect_to_candidate_AP(&wifi);
    if (RTW_SUCCESS == ret) {
        goto wifi_connect_success;
    } else {
        ret = SC_JOIN_BSS_FAIL;
        goto wifi_connect_fail;
    }

wifi_connect_success:
    ret = (enum sc_result)_SC_check_and_show_connection_info();
    goto wifi_connect_end;

wifi_connect_fail:
    printf("SC_connect_to_AP failed\n");
    goto wifi_connect_end;

wifi_connect_end:
#if CONFIG_AUTO_RECONNECT
    wifi_config_autoreconnect(1, 10, 5);
#endif
    return ret;
}

static void _SC_set_ack_content(void)
{
    memset(&sg_ack_content, 0, sizeof(struct ack_msg));
    sg_ack_content.flag   = 0x20;
    sg_ack_content.length = htons(sizeof(struct ack_msg) - 3);
    memcpy(sg_ack_content.smac, xnetif[0].hwaddr, 6);
    sg_ack_content.status      = 0;
    sg_ack_content.device_type = 0;
#if LWIP_VERSION_MAJOR >= 2
    sg_ack_content.device_ip = ip4_addr_get_u32(ip_2_ip4(netif_ip_addr4(&xnetif[0])));
#else
    sg_ack_content.device_ip = xnetif[0].ip_addr.addr;
#endif
    memset(sg_ack_content.device_name, 0, 64);
}

static int _SC_send_simple_config_ack(u8 round)
{
#if CONFIG_LWIP_LAYER
    int ack_transmit_round, ack_num_each_sec;
    int ack_socket;
    // int sended_data = 0;
    struct sockaddr_in to_addr;
#if LEAVE_ACK_EARLY
    uint8_t check_phone_ack = 0;
#endif
    _SC_set_ack_content();

    ack_socket = socket(PF_INET, SOCK_DGRAM, IP_PROTO_UDP);
    if (ack_socket == -1) {
        return -1;
    }
#if LEAVE_ACK_EARLY
    else {
        struct sockaddr_in bindAddr;
        bindAddr.sin_family      = AF_INET;
        bindAddr.sin_port        = htons(8864);
        bindAddr.sin_addr.s_addr = INADDR_ANY;
        if (bind(ack_socket, (struct sockaddr *)&bindAddr, sizeof(bindAddr)) == 0)
            check_phone_ack = 1;
    }
#endif

    // else {
    //     struct sockaddr_in bindAddr;
    //     bindAddr.sin_family      = AF_INET;
    //     bindAddr.sin_port        = htons(8863);
    //     bindAddr.sin_addr.s_addr = INADDR_ANY;
    //     if (bind(ack_socket, (struct sockaddr *)&bindAddr, sizeof(bindAddr)) == 0) {
    //         Log_i("bind success");
    //     } else {
    //         Log_e("unable to bind");
    //     }
    // }

    // Log_i("get simple config ip from mini program...");
    // unsigned char packet[100];
    // int packetLen;
    // struct sockaddr from;
    // struct sockaddr_in *from_sin = (struct sockaddr_in *)&from;
    // socklen_t fromLen            = sizeof(from);

    // while (1) {
    //     if ((packetLen = recvfrom(ack_socket, &packet, sizeof(packet), MSG_DONTWAIT, &from, &fromLen)) >= 0) {
    //         uint8_t *ip        = (uint8_t *)&from_sin->sin_addr.s_addr;
    //         uint16_t from_port = ntohs(from_sin->sin_port);
    //         printf("recv %d bytes from %d.%d.%d.%d:%d at round=%d, num=%d\n", packetLen, ip[0], ip[1], ip[2], ip[3],
    //                from_port, ack_transmit_round, ack_num_each_sec);
    //         goto send_ack;
    //     }
    //     vTaskDelay(50);
    // }

send_ack:
    Log_i("Sending simple config ack");
    FD_ZERO(&to_addr);
    to_addr.sin_family = AF_INET;
    to_addr.sin_port   = htons(8864);
    // to_addr.sin_addr.s_addr = (sg_backup_sc_ctx.ip_addr);
    to_addr.sin_addr.s_addr = inet_addr("255.255.255.255");
    for (ack_transmit_round = 0; ack_transmit_round < round; ack_transmit_round++) {
        for (ack_num_each_sec = 0; ack_num_each_sec < 20; ack_num_each_sec++) {
            // sended_data =
            sendto(ack_socket, (unsigned char *)&sg_ack_content, sizeof(struct ack_msg), 0, (struct sockaddr *)&to_addr,
                   sizeof(struct sockaddr));
            // printf("\r\nAlready send %d bytes data\n", sended_data);
            vTaskDelay(50); /* delay 50 ms */

#if LEAVE_ACK_EARLY
            if (check_phone_ack) {
                unsigned char packet[100];
                int packetLen;
                struct sockaddr from;
                struct sockaddr_in *from_sin = (struct sockaddr_in *)&from;
                socklen_t fromLen            = sizeof(from);

                if ((packetLen = recvfrom(ack_socket, &packet, sizeof(packet), MSG_DONTWAIT, &from, &fromLen)) >= 0) {
                    uint8_t *ip        = (uint8_t *)&from_sin->sin_addr.s_addr;
                    uint16_t from_port = ntohs(from_sin->sin_port);
                    printf("recv %d bytes from %d.%d.%d.%d:%d at round=%d, num=%d\n", packetLen, ip[0], ip[1], ip[2],
                           ip[3], from_port, ack_transmit_round, ack_num_each_sec);
                    goto leave_ack;
                }
            }
#endif
        }
    }

leave_ack:
    close(ack_socket);
#endif
    return 0;
}

//============================ connect to AP function end ===========================//

static void _simple_config_channel_control(void *para)
{
    uint32_t start_time;
    uint32_t current_time;

    WifiConfigEventCallBack wifi_config_event_cb = para;

    int is_timeout       = 0;
    int ch_idx           = 0;
    int fix_channel      = 0;
    int delta_time       = -20;
    int is_fixed_channel = 0;

    int channel_nums = sizeof(sg_simple_config_promisc_channel_tbl) / sizeof(sg_simple_config_promisc_channel_tbl[0]);

    start_time = xTaskGetTickCount();

    while (!sg_simple_config_terminate) {
        vTaskDelay(50);  // delay 0.5s to release CPU usage

        if (xTaskGetTickCount() - sg_cmd_start_time < ((120 + delta_time) * configTICK_RATE_HZ)) {
            current_time = xTaskGetTickCount();
            if (((current_time - start_time) * 1000 / configTICK_RATE_HZ < GET_CHANNEL_INTERVAL) || is_fixed_channel) {
                if (!is_fixed_channel && get_channel_flag == 1) {
                    fix_channel = promisc_get_fixed_channel(g_bssid, sg_ssid, &sg_ssid_len);
                    if (fix_channel != 0) {
                        Log_i("in simple_config_test fix channel = %d ssid: %s", fix_channel, sg_ssid);
                        is_fixed_channel     = 1;
                        sg_fixed_channel_num = fix_channel;
                        wifi_set_channel(fix_channel);
                    } else {
                        Log_e("get channel fail!");
                    }
                }

                if (sg_simple_config_result == 1) {
                    is_fixed_channel         = 0;
                    sg_is_need_connect_to_AP = 1;
                    break;
                }

                if (sg_simple_config_result == -1) {
                    Log_e("simple_config_test restart for result = -1!");
                    delta_time = 60;
                    wifi_set_channel(1);
                    is_fixed_channel = 0;

                    sg_is_need_connect_to_AP = 0;
                    sg_fixed_channel_num     = 0;

                    memset(sg_ssid, 0, 33);
                    sg_ssid_len             = 0;
                    sg_simple_config_result = 0;

                    g_security_mode = 0xff;
                    rtk_restart_simple_config();
                }
            } else {
                ch_idx++;
                ch_idx = (ch_idx >= channel_nums) ? 0 : ch_idx;
                if (wifi_set_channel(sg_simple_config_promisc_channel_tbl[ch_idx]) == 0) {
                    start_time = xTaskGetTickCount();
                    Log_i("Switch to channel(%d)", sg_simple_config_promisc_channel_tbl[ch_idx]);
                }
            }
        } else {
            is_timeout = 1;
            break;
        }
    }  // end of while

    if (is_promisc_enabled()) {
        wifi_set_promisc(RTW_PROMISC_DISABLE, NULL, 0);
    }

    if (sg_is_need_connect_to_AP == 1) {
        if (SC_SUCCESS == _SC_connect_to_AP()) {
            if (0 == _SC_send_simple_config_ack(30)) {
                wifi_config_event_cb(EVENT_WIFI_CONFIG_SUCCESS, NULL);
                goto exit;
            }
        }
    }

    if (is_timeout) {
        wifi_config_event_cb(EVENT_WIFI_CONFIG_TIMEOUT, NULL);
    } else {
        wifi_config_event_cb(EVENT_WIFI_CONFIG_FAILED, NULL);
    }

exit:
    rtw_up_sema(&sg_simple_config_finish_sema);
    vTaskDelete(NULL);
    return;
}

static int _simple_config_task_start(void *params)
{
    if (wifi_set_promisc(RTW_PROMISC_ENABLE, _simple_config_callback, 1)) {
        Log_e("Set promisc mode failed!");
        return -1;
    }

    if (xTaskCreate(_simple_config_channel_control, ((const char *)"simple_config_channel_control"), 1024, params,
                    tskIDLE_PRIORITY + 5, NULL) != pdPASS) {
        Log_e("xTaskCreate(simple_config_channel_control) failed!");
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
int simple_config_start(void *params, WifiConfigEventCallBack event_cb)
{
    int ret = 0;

    // 1. entering promisc mode
    if (wifi_set_mode(RTW_MODE_PROMISC)) {
        Log_e("Promisc mode is running!");
        return -1;
    }

    // 2. init simple config
    if (params) {
        ret = _init_simple_config(((eSimpleConfigParams *)params)->custom_pin_code);
    } else {
        // without pin code, it will using default:57289961
        ret = _init_simple_config(NULL);
    }

    if (0 == ret) {
        Log_d("simple config init success!");

        // 3. add filter
        _filter_add_enable();

        // 4. start simple config task
        ret = _simple_config_task_start(event_cb);
        if (ret) {
            _deinit_simple_config();
            _filter_remove();
        }

        return ret;
    }

    return -1;
}

/**
 * @brief Stop WiFi config and device binding process
 */
void simple_config_stop(void)
{
    sg_simple_config_terminate = 1;
    if (rtw_down_sema(&sg_simple_config_finish_sema) == _FAIL) {
        Log_e("Take Semaphore Fail!");
    }
    _deinit_simple_config();
    _filter_remove();
}
