/*
 * SPDX-FileCopyrightText: 2021 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#ifndef __APP_HF_MSG_SET_H__
#define __APP_HF_MSG_SET_H__

#define HF_MSG_ARGS_MAX             (8)

typedef int (* hf_cmd_handler)(int argn, char **argv);

typedef struct {
    const char *str;
    hf_cmd_handler handler;
} hf_msg_hdl_t;

// Audio state update functions
void hf_audio_state_connecting(void);
void hf_audio_state_connected(void);
void hf_audio_state_disconnected(void);

void register_hfp_ag(void);
#endif /* __APP_HF_MSG_SET_H__*/
