/*
 * SPDX-FileCopyrightText: 2021-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#ifndef __BT_APP_HF_H__
#define __BT_APP_HF_H__

#include <stdint.h>
#include "esp_hf_ag_api.h"
#include "esp_bt_defs.h"

extern esp_bd_addr_t hf_peer_addr; // Declaration of peer device bdaddr

#define BT_HF_TAG               "BT_APP_HF"

/**
 * @brief     callback function for HF client
 */
void bt_app_hf_cb(esp_hf_cb_event_t event, esp_hf_cb_param_t *param);

/**
 * @brief     start microphone level monitoring
 */
void bt_app_start_mic_level_monitoring(void);

/**
 * @brief     stop microphone level monitoring
 */
void bt_app_stop_mic_level_monitoring(void);

/**
 * @brief     initialize audio streaming to server
 */
esp_err_t bt_app_audio_streaming_init(const char *server_ip, uint16_t server_port);

/**
 * @brief     start audio streaming
 */
esp_err_t bt_app_audio_streaming_start(void);

/**
 * @brief     stop audio streaming
 */
esp_err_t bt_app_audio_streaming_stop(void);

/**
 * @brief     deinitialize audio streaming
 */
esp_err_t bt_app_audio_streaming_deinit(void);

/**
 * @brief     check if audio streaming is connected
 */
bool bt_app_audio_streaming_is_connected(void);

#endif /* __BT_APP_HF_H__*/
