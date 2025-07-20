/*
 * SPDX-FileCopyrightText: 2021-2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <stdio.h>
#include <string.h>
#include "esp_hf_ag_api.h"
#include "app_hf_msg_set.h"
#include "bt_app_hf.h"
#include "esp_console.h"
#include "argtable3/argtable3.h"
#include "esp_log.h"
#include "call_simulation.h" // Добавляем новый заголовочный файл
#include "wifi_manager.h"
#include "autostart.h"

// if you want to connect a specific device, add it's bda here
esp_bd_addr_t hf_peer_addr = {0xB0, 0xF1, 0xA3, 0x01, 0x2D,0x2E};

// Global audio state tracking (убираем static, чтобы переменные были доступны из других файлов)
bool g_audio_connecting = false;
bool g_audio_connected = false;

// Functions to update audio state (called from bt_app_hf.c)
void hf_audio_state_connecting(void) {
    g_audio_connecting = true;
    g_audio_connected = false;
}

void hf_audio_state_connected(void) {
    g_audio_connecting = false;
    g_audio_connected = true;

    // Автоматически запускаем мониторинг уровня сигнала при подключении аудио
    printf("Audio connected - starting microphone level monitoring automatically.\n");

    // Активируем голосовое распознавание для запуска микрофонного потока
    printf("Activating voice recognition to start microphone stream...\n");
    esp_hf_ag_vra_control(hf_peer_addr, 1);

    bt_app_start_mic_level_monitoring();
}

void hf_audio_state_disconnected(void) {
    g_audio_connecting = false;
    g_audio_connected = false;

    // Останавливаем мониторинг при отключении аудио
    printf("Audio disconnected - stopping microphone level monitoring.\n");
    bt_app_stop_mic_level_monitoring();
}

// Function to parse MAC address from string to esp_bd_addr_t
static bool parse_mac_address(const char* mac_str, esp_bd_addr_t addr) {
    int values[6];
    int result = sscanf(mac_str, "%x:%x:%x:%x:%x:%x",
                       &values[0], &values[1], &values[2],
                       &values[3], &values[4], &values[5]);

    if (result == 6) {
        for (int i = 0; i < 6; i++) {
            addr[i] = (uint8_t)values[i];
        }
        return true;
    }
    return false;
}

#define HF_CMD_HANDLER(cmd)    static int hf_##cmd##_handler(int argn, char **argv)

HF_CMD_HANDLER(conn)
{
    if (argn < 2) {
        printf("Usage: con <MAC_ADDRESS>\n");
        printf("Example: con BC:F2:92:AE:91:F0\n");
        return 1;
    }

    esp_bd_addr_t target_addr;
    if (!parse_mac_address(argv[1], target_addr)) {
        printf("Invalid MAC address format. Use format: XX:XX:XX:XX:XX:XX\n");
        return 1;
    }

    // Update the peer address
    memcpy(hf_peer_addr, target_addr, ESP_BD_ADDR_LEN);

    printf("Connecting to %s...\n", argv[1]);
    esp_hf_ag_slc_connect(hf_peer_addr);
    return 0;
}

HF_CMD_HANDLER(disc)
{
    printf("Disconnect\n");
    esp_hf_ag_slc_disconnect(hf_peer_addr);
    return 0;
}

HF_CMD_HANDLER(conn_audio)
{
    printf("Connect Audio\n");
    esp_hf_ag_audio_connect(hf_peer_addr);
    return 0;
}

HF_CMD_HANDLER(disc_audio)
{
    printf("Disconnect Audio\n");
    esp_hf_ag_audio_disconnect(hf_peer_addr);
    return 0;
}

//AT+BVRA
HF_CMD_HANDLER(vra_on)
{
    printf("Start Voice Recognition.\n");
    esp_hf_ag_vra_control(hf_peer_addr,1);
    return 0;
}
//AT+BVRA
HF_CMD_HANDLER(vra_off)
{
    printf("Stop Voicer Recognition.\n");
    esp_hf_ag_vra_control(hf_peer_addr,0);
    return 0;
}

//AT+VGS or AT+VGM
HF_CMD_HANDLER(volume_control)
{
    if (argn != 3) {
        printf("Insufficient number of arguments");
        return 1;
    }
    int target, volume;
    if (sscanf(argv[1], "%d", &target) != 1 ||
        (target != ESP_HF_VOLUME_CONTROL_TARGET_SPK &&
        target != ESP_HF_VOLUME_CONTROL_TARGET_MIC)) {
        printf("Invalid argument for target %s\n", argv[1]);
        return 1;
    }
    if (sscanf(argv[2], "%d", &volume) != 1 ||
            (volume < 0 || volume > 15)) {
        printf("Invalid argument for volume %s\n", argv[2]);
        return 1;
    }
    printf("Volume Update\n");
    esp_hf_ag_volume_control(hf_peer_addr, target, volume);
    return 0;
}

//+CIEV
HF_CMD_HANDLER(ciev_report)
{
    if (argn != 3) {
        printf("Insufficient number of arguments");
        return 1;
    }

    int ind_type, value;

    sscanf(argv[1], "%d", &ind_type);
    sscanf(argv[2], "%d", &value);

    if (ind_type > ESP_HF_IND_TYPE_CALLHELD) {
        printf("Invalid argument for status type %s\n", argv[1]);
        return 1;
    }
    if ((ind_type == ESP_HF_IND_TYPE_CALL) &&
        (value != ESP_HF_CALL_STATUS_NO_CALLS &&
        value != ESP_HF_CALL_STATUS_CALL_IN_PROGRESS)) {
        printf("Invalid argument for callsetup state %s\n", argv[2]);
        return 1;
    }
    if ((ind_type == ESP_HF_IND_TYPE_CALLSETUP) &&
        (value < ESP_HF_CALL_SETUP_STATUS_IDLE ||
        value > ESP_HF_CALL_SETUP_STATUS_OUTGOING_ALERTING)) {
        printf("Invalid argument for call state %s\n", argv[2]);
        return 1;
    }
    if ((ind_type == ESP_HF_IND_TYPE_SERVICE) &&
        (value != ESP_HF_NETWORK_STATE_NOT_AVAILABLE &&
        value != ESP_HF_NETWORK_STATE_AVAILABLE)) {
        printf("Invalid argument for network state %s\n", argv[2]);
        return 1;
    }
    if ((ind_type == ESP_HF_IND_TYPE_SIGNAL &&
        (value < 0 || value > 5))) {
        printf("Invalid argument for signal %s\n", argv[2]);
        return 1;
    }
    if ((ind_type == ESP_HF_IND_TYPE_ROAM &&
        (value != ESP_HF_ROAMING_STATUS_INACTIVE &&
        value != ESP_HF_ROAMING_STATUS_ACTIVE))) {
        printf("Invalid argument for roaming state %s\n", argv[2]);
        return 1;
    }
    if ((ind_type == ESP_HF_IND_TYPE_BATTCHG &&
        (value < 0 || value > 5))) {
        printf("Invalid argument for battery %s\n", argv[2]);
        return 1;
    }
    if ((ind_type == ESP_HF_IND_TYPE_CALLHELD) &&
        (value < ESP_HF_CALL_HELD_STATUS_NONE ||
        value > ESP_HF_CALL_HELD_STATUS_HELD)) {
        printf("Invalid argument for call held state %s\n", argv[2]);
        return 1;
    }

    printf("Device Indicator Changed!\n");
    esp_hf_ag_ciev_report(hf_peer_addr, ind_type, value);
    return 0;
}

//AT+CMEE
HF_CMD_HANDLER(cme_err)
{
    if (argn != 3) {
        printf("Insufficient number of arguments");
        return 1;
    }

    int response_code, error_code;
    if (sscanf(argv[1], "%d", &response_code) != 1 ||
        (response_code < ESP_HF_AT_RESPONSE_CODE_OK && response_code > ESP_HF_AT_RESPONSE_CODE_CME)) {
        printf("Invalid argument for response_code %s\n", argv[1]);
        return 1;
    }

    if (sscanf(argv[2], "%d", &error_code) != 1 ||
            (error_code < ESP_HF_CME_AG_FAILURE || error_code > ESP_HF_CME_NETWORK_NOT_ALLOWED)) {
        printf("Invalid argument for volume %s\n", argv[2]);
        return 1;
    }

    printf("Send CME Error.\n");
    esp_hf_ag_cmee_send(hf_peer_addr,response_code,error_code);
    return 0;
}

//+BSIR:1
HF_CMD_HANDLER(ir_on)
{
    printf("Enable Voicer Recognition.\n");
    esp_hf_ag_bsir(hf_peer_addr,1);
    return 0;
}

//+BSIR:0
HF_CMD_HANDLER(ir_off)
{
    printf("Disable Voicer Recognition.\n");
    esp_hf_ag_bsir(hf_peer_addr,0);
    return 0;
}

//Answer Call from AG
HF_CMD_HANDLER(ac)
{
    printf("Answer Call from AG.\n");
    char *number = {"123456"};
    esp_hf_ag_answer_call(hf_peer_addr,1,0,1,1,number,0);
    return 0;
}

//Reject Call from AG
HF_CMD_HANDLER(rc)
{
    printf("Reject Call from AG.\n");
    char *number = {"123456"};
    esp_hf_ag_reject_call(hf_peer_addr,0,0,0,0,number,0);
    return 0;
}

//End Call from AG
HF_CMD_HANDLER(end)
{
    printf("End Call from AG.\n");
    char *number = {"123456"};
    esp_hf_ag_end_call(hf_peer_addr,0,0,0,0,number,0);
    return 0;
}

//Dial Call from AG
HF_CMD_HANDLER(dn)
{
    if (argn != 2) {
        printf("Insufficient number of arguments");
    } else {
        printf("Dial number %s\n", argv[1]);
        esp_hf_ag_out_call(hf_peer_addr,1,0,1,2,argv[1],0);
    }
    return 0;
}

//Monitor Microphone Level
HF_CMD_HANDLER(mic_level)
{
    printf("Start microphone level monitoring.\n");

    // Запускаем мониторинг уровня микрофона
    esp_err_t ret = start_microphone_level_monitoring();
    if (ret != ESP_OK) {
        printf("Failed to start microphone level monitoring: %s\n", esp_err_to_name(ret));
        return 1;
    }

    // Всегда запускаем мониторинг независимо от текущего состояния аудио
    bt_app_start_mic_level_monitoring();

    // Проверяем, активно ли уже аудиосоединение
    if (g_audio_connected) {
        printf("✓ Audio connection already established.\n");
        printf("✓ Microphone level monitoring is now active.\n");
        printf("Use 'disa' to disconnect audio when done.\n");
        return 0;
    }

    if (g_audio_connecting) {
        printf("Audio connection is in progress... Monitoring will activate when connected.\n");
        printf("Watch for 'connected' or 'connected_msbc' status in the logs.\n");
        return 0;
    }

    // Если аудио не подключено, используем правильный API для активации голосового распознавания
    printf("Activating voice recognition to start microphone stream...\n");
    esp_hf_ag_vra_control(hf_peer_addr, 1);  // Включаем голосовое распознавание

    printf("Voice recognition activated - this should trigger audio connection.\n");
    printf("Watch for AUDIO_STATE_EVT messages in the log for connection status.\n");

    return 0;
}

//Check Audio Status
HF_CMD_HANDLER(audio_status)
{
    printf("Audio connection status check:\n");
    printf("- Check logs for latest AUDIO_STATE_EVT messages\n");
    printf("- 'connecting' = establishing connection\n");
    printf("- 'connected' = CVSD codec (8kHz)\n");
    printf("- 'connected_msbc' = mSBC codec (16kHz)\n");
    printf("- 'disconnected' = no audio connection\n");
    return 0;
}

//Start Call Simulation with auto-answer
HF_CMD_HANDLER(call_start)
{
    printf("Starting incoming call simulation...\n");
    esp_err_t ret = start_incoming_call_simulation(true);
    if (ret == ESP_OK) {
        printf("Call simulation started. Will auto-answer in 2 seconds.\n");
        printf("This should activate microphone on the headset.\n");
    } else {
        printf("Failed to start call simulation: %s\n", esp_err_to_name(ret));
    }
    return 0;
}

//Answer Simulated Call
HF_CMD_HANDLER(call_answer)
{
    printf("Answering simulated call...\n");
    esp_err_t ret = answer_simulated_call();
    if (ret == ESP_OK) {
        printf("Call answered. Microphone should now be active.\n");
    } else {
        printf("Failed to answer call: %s\n", esp_err_to_name(ret));
    }
    return 0;
}

//End Simulated Call
HF_CMD_HANDLER(call_end)
{
    printf("Ending simulated call...\n");
    esp_err_t ret = end_simulated_call();
    if (ret == ESP_OK) {
        printf("Call ended.\n");
    } else {
        printf("Failed to end call: %s\n", esp_err_to_name(ret));
    }
    return 0;
}

//Force Audio Data Request
HF_CMD_HANDLER(force_audio)
{
    printf("Forcing audio data request...\n");
    printf("This will try to trigger incoming audio callback.\n");

    // Попробуем принудительно вызвать готовность к приему данных
    if (g_audio_connected) {
        printf("Audio is connected, requesting data...\n");
        // Попробуем различные способы активации
        esp_hf_ag_vra_control(hf_peer_addr, 1);  // Voice recognition ON
        esp_hf_ag_volume_control(hf_peer_addr, ESP_HF_VOLUME_CONTROL_TARGET_MIC, 15); // Max mic volume
        printf("Voice recognition enabled and mic volume maximized.\n");
        printf("Try speaking into the headset microphone.\n");
    } else {
        printf("Audio connection not established. Use 'miclevel' or 'call_start' first.\n");
    }
    return 0;
}

//Stop Microphone Monitoring
HF_CMD_HANDLER(stop_mic)
{
    printf("Stopping microphone level monitoring...\n");
    bt_app_stop_mic_level_monitoring();
    printf("Microphone monitoring stopped.\n");
    return 0;
}

// Audio Streaming Commands

//Initialize Audio Streaming
HF_CMD_HANDLER(stream_init)
{
    if (argn != 3) {
        printf("Usage: stream_init <server_ip> <port>\n");
        printf("Example: stream_init 192.168.1.100 8888\n");
        return 1;
    }

    const char *server_ip = argv[1];
    int port = atoi(argv[2]);

    if (port <= 0 || port > 65535) {
        printf("Invalid port number: %s\n", argv[2]);
        return 1;
    }

    printf("Initializing audio streaming to %s:%d...\n", server_ip, port);
    esp_err_t ret = bt_app_audio_streaming_init(server_ip, (uint16_t)port);

    if (ret == ESP_OK) {
        printf("✅ Audio streaming initialized successfully\n");
        printf("💡 Use 'stream_start' to begin streaming\n");
    } else {
        printf("❌ Failed to initialize audio streaming: %s\n", esp_err_to_name(ret));
    }

    return 0;
}

//Start Audio Streaming
HF_CMD_HANDLER(stream_start)
{
    printf("Starting audio streaming...\n");
    esp_err_t ret = bt_app_audio_streaming_start();

    if (ret == ESP_OK) {
        printf("🎵 Audio streaming started\n");
        printf("💡 Audio data will be sent to server when microphone is active\n");
    } else {
        printf("❌ Failed to start audio streaming: %s\n", esp_err_to_name(ret));
    }

    return 0;
}

//Stop Audio Streaming
HF_CMD_HANDLER(stream_stop)
{
    printf("Stopping audio streaming...\n");
    esp_err_t ret = bt_app_audio_streaming_stop();

    if (ret == ESP_OK) {
        printf("⏹️ Audio streaming stopped\n");
    } else {
        printf("❌ Failed to stop audio streaming: %s\n", esp_err_to_name(ret));
    }

    return 0;
}

//Audio Streaming Status
HF_CMD_HANDLER(stream_status)
{
    bool connected = bt_app_audio_streaming_is_connected();

    printf("📊 Audio Streaming Status:\n");
    printf("  Server connection: %s\n", connected ? "✅ Connected" : "❌ Disconnected");
    printf("  Audio state: %s\n", g_audio_connected ? "✅ Connected" : "❌ Disconnected");
    printf("  Bluetooth state: %s\n", g_audio_connecting ? "🔄 Connecting" :
           (g_audio_connected ? "✅ Connected" : "❌ Disconnected"));

    if (connected && g_audio_connected) {
        printf("🎤 Ready to stream microphone data\n");
    } else if (!connected) {
        printf("💡 Use 'stream_init <ip> <port>' and 'stream_start' to begin\n");
    } else if (!g_audio_connected) {
        printf("💡 Connect Bluetooth audio to start streaming\n");
    }

    return 0;
}

// WiFi Management Commands

//Connect to WiFi
HF_CMD_HANDLER(wifi_connect)
{
    if (argn < 2 || argn > 3) {
        printf("Usage: wifi_connect <ssid> [password]\n");
        printf("Example: wifi_connect MyWiFi mypassword\n");
        printf("Example: wifi_connect OpenWiFi\n");
        return 1;
    }

    const char *ssid = argv[1];
    const char *password = (argn == 3) ? argv[2] : "";

    printf("Connecting to WiFi: %s...\n", ssid);
    esp_err_t ret = wifi_manager_connect(ssid, password);

    if (ret == ESP_OK) {
        char ip_str[16];
        wifi_manager_get_ip(ip_str, sizeof(ip_str));
        printf("✅ WiFi connected successfully\n");
        printf("📡 IP Address: %s\n", ip_str);
        printf("💡 Now you can use audio streaming commands\n");
    } else {
        printf("❌ Failed to connect to WiFi: %s\n", esp_err_to_name(ret));
        printf("💡 Check SSID and password, then try again\n");
    }

    return 0;
}

//Disconnect WiFi
HF_CMD_HANDLER(wifi_disconnect)
{
    printf("Disconnecting from WiFi...\n");
    esp_err_t ret = wifi_manager_disconnect();

    if (ret == ESP_OK) {
        printf("📴 WiFi disconnected\n");
    } else {
        printf("❌ Failed to disconnect: %s\n", esp_err_to_name(ret));
    }

    return 0;
}

//WiFi Status
HF_CMD_HANDLER(wifi_status)
{
    bool connected = wifi_manager_is_connected();

    printf("📶 WiFi Status:\n");
    printf("  Connection: %s\n", connected ? "✅ Connected" : "❌ Disconnected");

    if (connected) {
        char ip_str[16];
        esp_err_t ret = wifi_manager_get_ip(ip_str, sizeof(ip_str));
        if (ret == ESP_OK) {
            printf("  IP Address: %s\n", ip_str);
            printf("🎵 Ready for audio streaming\n");
        }
    } else {
        printf("💡 Use 'wifi_connect <ssid> [password]' to connect\n");
    }

    return 0;
}

// Autostart Management Commands

//Set Autostart Commands
HF_CMD_HANDLER(autostart_set)
{
    if (argn < 2) {
        printf("Usage: autostart_set <command1> [command2] [...]\n");
        printf("Example: autostart_set \"wifi_connect MyWiFi password\" \"stream_init 192.168.1.100 8888\" \"stream_start\"\n");
        printf("Note: Use quotes for commands with spaces\n");
        return 1;
    }

    // Создаем массив команд из аргументов
    const char **commands = malloc((argn - 1) * sizeof(char*));
    if (!commands) {
        printf("Failed to allocate memory for commands\n");
        return 1;
    }

    for (int i = 1; i < argn; i++) {
        commands[i - 1] = argv[i];
    }

    esp_err_t ret = autostart_save_commands(commands, argn - 1);
    free(commands);

    if (ret == ESP_OK) {
        printf("✅ Autostart commands saved (%d commands)\n", argn - 1);
        printf("Commands will be executed on next boot if autostart is enabled\n");
        for (int i = 1; i < argn; i++) {
            printf("  %d. %s\n", i, argv[i]);
        }
    } else {
        printf("❌ Failed to save autostart commands: %s\n", esp_err_to_name(ret));
    }

    return 0;
}

//Load Default Autostart Commands
HF_CMD_HANDLER(autostart_load_default)
{
    printf("Loading default autostart commands...\n");

    // Команды из вашего файла command.txt
    const char *default_commands[] = {
        "wifi_connect Keenetic-6786 9811992776",
        "stream_init 192.168.1.169 8888",
        "stream_start"
    };
    size_t num_commands = sizeof(default_commands) / sizeof(default_commands[0]);

    esp_err_t ret = autostart_save_commands(default_commands, num_commands);

    if (ret == ESP_OK) {
        printf("✅ Default autostart commands loaded:\n");
        for (size_t i = 0; i < num_commands; i++) {
            printf("  %zu. %s\n", i + 1, default_commands[i]);
        }
        printf("Commands will be executed on next boot if autostart is enabled\n");
    } else {
        printf("❌ Failed to load default commands: %s\n", esp_err_to_name(ret));
    }

    return 0;
}

//Show Autostart Commands
HF_CMD_HANDLER(autostart_show)
{
    printf("📋 Autostart Configuration:\n");
    printf("Status: %s\n", autostart_is_enabled() ? "✅ Enabled" : "❌ Disabled");

    char **commands = NULL;
    size_t count = 0;
    esp_err_t ret = autostart_load_commands(&commands, &count);

    if (ret != ESP_OK) {
        printf("❌ Failed to load commands: %s\n", esp_err_to_name(ret));
        return 1;
    }

    if (count == 0) {
        printf("Commands: (none configured)\n");
        printf("💡 Use 'autostart_set' or 'autostart_load_default' to configure commands\n");
    } else {
        printf("Commands (%zu configured):\n", count);
        for (size_t i = 0; i < count; i++) {
            printf("  %zu. %s\n", i + 1, commands[i]);
            free(commands[i]);
        }
        free(commands);
    }

    return 0;
}

//Enable/Disable Autostart
HF_CMD_HANDLER(autostart_enable)
{
    if (argn != 2) {
        printf("Usage: autostart_enable <0|1>\n");
        printf("  0 = disable autostart\n");
        printf("  1 = enable autostart\n");
        return 1;
    }

    int enable = atoi(argv[1]);
    if (enable != 0 && enable != 1) {
        printf("Invalid argument. Use 0 (disable) or 1 (enable)\n");
        return 1;
    }

    esp_err_t ret = autostart_set_enabled(enable != 0);
    if (ret == ESP_OK) {
        printf("✅ Autostart %s\n", enable ? "enabled" : "disabled");
        if (enable) {
            printf("Commands will be executed automatically on next boot\n");
        }
    } else {
        printf("❌ Failed to update autostart setting: %s\n", esp_err_to_name(ret));
    }

    return 0;
}

//Clear Autostart Commands
HF_CMD_HANDLER(autostart_clear)
{
    printf("Clearing autostart commands...\n");
    esp_err_t ret = autostart_clear();

    if (ret == ESP_OK) {
        printf("✅ Autostart commands cleared\n");
        printf("Autostart is now disabled\n");
    } else {
        printf("❌ Failed to clear autostart commands: %s\n", esp_err_to_name(ret));
    }

    return 0;
}

//Execute Autostart Commands Now
HF_CMD_HANDLER(autostart_run)
{
    printf("🚀 Executing autostart commands now...\n");
    esp_err_t ret = autostart_execute();

    if (ret == ESP_OK) {
        printf("✅ Autostart execution completed\n");
    } else {
        printf("❌ Autostart execution failed: %s\n", esp_err_to_name(ret));
    }

    return 0;
}
static hf_msg_hdl_t hf_cmd_tbl[] = {
    {"con",          hf_conn_handler},
    {"dis",          hf_disc_handler},
    {"cona",         hf_conn_audio_handler},
    {"disa",         hf_disc_audio_handler},
    {"vu",           hf_volume_control_handler},
    {"ciev",         hf_ciev_report_handler},
    {"vron",         hf_vra_on_handler},
    {"vroff",        hf_vra_off_handler},
    {"ate",          hf_cme_err_handler},
    {"iron",         hf_ir_on_handler},
    {"iroff",        hf_ir_off_handler},
    {"ac",           hf_ac_handler},
    {"rc",           hf_rc_handler},
    {"end",          hf_end_handler},
    {"dn",           hf_dn_handler},
    {"miclevel",     hf_mic_level_handler},
    {"audiostatus",  hf_audio_status_handler},
    {"call_start",   hf_call_start_handler},
    {"call_answer",  hf_call_answer_handler},
    {"call_end",     hf_call_end_handler},
    {"force_audio",  hf_force_audio_handler},
    {"stop_mic",     hf_stop_mic_handler},
    {"stream_init",  hf_stream_init_handler},
    {"stream_start", hf_stream_start_handler},
    {"stream_stop",  hf_stream_stop_handler},
    {"stream_status",hf_stream_status_handler},
    {"wifi_connect",  hf_wifi_connect_handler},
    {"wifi_disconnect",hf_wifi_disconnect_handler},
    {"wifi_status",  hf_wifi_status_handler},
    {"autostart_set", hf_autostart_set_handler},
    {"autostart_load_default", hf_autostart_load_default_handler},
    {"autostart_show", hf_autostart_show_handler},
    {"autostart_enable", hf_autostart_enable_handler},
    {"autostart_clear", hf_autostart_clear_handler},
    {"autostart_run", hf_autostart_run_handler},
};

#define HF_ORDER(name)   name##_cmd
enum hf_cmd_idx {
    HF_CMD_IDX_CON = 0,       /*set up connection with peer device*/
    HF_CMD_IDX_DIS,           /*disconnection with peer device*/
    HF_CMD_IDX_CONA,          /*set up audio connection with peer device*/
    HF_CMD_IDX_DISA,          /*release audio connection with peer device*/
    HF_CMD_IDX_VU,            /*volume update*/
    HF_CMD_IDX_CIEV,          /*unsolicited indication device status to HF Client*/
    HF_CMD_IDX_VRON,          /*start voice recognition*/
    HF_CMD_IDX_VROFF,         /*stop voice recognition*/
    HF_CMD_IDX_ATE,           /*send extended AT error code*/
    HF_CMD_IDX_IRON,          /*in-band ring tone provided*/
    HF_CMD_IDX_IROFF,         /*in-band ring tone not provided*/
    HF_CMD_IDX_AC,            /*Answer Incoming Call from AG*/
    HF_CMD_IDX_RC,            /*Reject Incoming Call from AG*/
    HF_CMD_IDX_END,           /*End up a call by AG*/
    HF_CMD_IDX_DN,            /*Dial Number by AG, e.g. d 11223344*/
    HF_CMD_IDX_MICLEVEL,      /*Monitor microphone level*/
    HF_CMD_IDX_AUDIOSTATUS,   /*Check audio connection status*/
    HF_CMD_IDX_CALL_START,    /*Start call simulation*/
    HF_CMD_IDX_CALL_ANSWER,   /*Answer simulated call*/
    HF_CMD_IDX_CALL_END,      /*End simulated call*/
    HF_CMD_IDX_FORCE_AUDIO,   /*Force audio data request*/
    HF_CMD_IDX_STOP_MIC,      /*Stop microphone monitoring*/
    HF_CMD_IDX_STREAM_INIT,   /*Initialize audio streaming*/
    HF_CMD_IDX_STREAM_START,  /*Start audio streaming*/
    HF_CMD_IDX_STREAM_STOP,   /*Stop audio streaming*/
    HF_CMD_IDX_STREAM_STATUS, /*Check audio streaming status*/
    HF_CMD_IDX_WIFI_CONNECT,  /*Connect to WiFi*/
    HF_CMD_IDX_WIFI_DISCONNECT, /*Disconnect from WiFi*/
    HF_CMD_IDX_WIFI_STATUS,    /*Check WiFi status*/
    HF_CMD_IDX_AUTOSTART_SET,  /*Set autostart commands*/
    HF_CMD_IDX_AUTOSTART_LOAD_DEFAULT, /*Load default autostart commands*/
    HF_CMD_IDX_AUTOSTART_SHOW,  /*Show autostart commands*/
    HF_CMD_IDX_AUTOSTART_ENABLE, /*Enable or disable autostart*/
    HF_CMD_IDX_AUTOSTART_CLEAR,  /*Clear autostart commands*/
    HF_CMD_IDX_AUTOSTART_RUN    /*Execute autostart commands now*/
};

static char *hf_cmd_explain[] = {
    "set up connection with peer device (usage: con <MAC_ADDRESS>)",
    "disconnection with peer device",
    "set up audio connection with peer device",
    "release audio connection with peer device",
    "volume update",
    "unsolicited indication device status to HF Client",
    "start voice recognition",
    "stop voice recognition",
    "send extended AT error code",
    "in-band ring tone provided",
    "in-band ring tone not provided",
    "Answer Incoming Call from AG",
    "Reject Incoming Call from AG",
    "End up a call by AG",
    "Dial Number by AG, e.g. d 11223344",
    "Monitor microphone level and establish audio connection",
    "Check audio connection status and print to console",
    "start call simulation with auto-answer",
    "answer the simulated call",
    "end the simulated call",
    "Force audio data request and trigger incoming audio callback",
    "Stop microphone level monitoring",
    "initialize audio streaming to a server (usage: stream_init <server_ip> <port>)",
    "start audio streaming to the server",
    "stop audio streaming",
    "check the status of audio streaming connection",
    "connect to a WiFi network (usage: wifi_connect <ssid> [password])",
    "disconnect from the current WiFi network",
    "check the status of WiFi connection",
    "set autostart commands (usage: autostart_set <command1> [command2] [...])",
    "load default autostart commands",
    "show autostart commands",
    "enable or disable autostart (usage: autostart_enable <0|1>)",
    "clear autostart commands",
    "execute autostart commands now",
};
typedef struct {
    struct arg_str *tgt;
    struct arg_str *vol;
    struct arg_end *end;
} vu_args_t;

typedef struct {
    struct arg_str *ind_type;
    struct arg_str *value;
    struct arg_end *end;
} ind_args_t;

typedef struct {
    struct arg_str *rep;
    struct arg_str *err;
    struct arg_end *end;
} ate_args_t;

static vu_args_t vu_args;
static ind_args_t ind_args;
static ate_args_t ate_args;

void register_hfp_ag(void)
{

        const esp_console_cmd_t HF_ORDER(con) = {
            .command = "con",
            .help = hf_cmd_explain[HF_CMD_IDX_CON],
            .hint = "<MAC_ADDRESS>",
            .func = hf_cmd_tbl[HF_CMD_IDX_CON].handler,
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&HF_ORDER(con)));

        const esp_console_cmd_t HF_ORDER(dis) = {
            .command = "dis",
            .help = hf_cmd_explain[HF_CMD_IDX_DIS],
            .hint = NULL,
            .func = hf_cmd_tbl[HF_CMD_IDX_DIS].handler,
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&HF_ORDER(dis)));

        const esp_console_cmd_t HF_ORDER(cona) = {
            .command = "cona",
            .help = hf_cmd_explain[HF_CMD_IDX_CONA],
            .hint = NULL,
            .func = hf_cmd_tbl[HF_CMD_IDX_CONA].handler,
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&HF_ORDER(cona)));

        const esp_console_cmd_t HF_ORDER(disa) = {
            .command = "disa",
            .help = hf_cmd_explain[HF_CMD_IDX_DISA],
            .hint = NULL,
            .func = hf_cmd_tbl[HF_CMD_IDX_DISA].handler,
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&HF_ORDER(disa)));

        const esp_console_cmd_t HF_ORDER(ac) = {
            .command = "ac",
            .help = hf_cmd_explain[HF_CMD_IDX_AC],
            .hint = NULL,
            .func = hf_cmd_tbl[HF_CMD_IDX_AC].handler,
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&HF_ORDER(ac)));

        const esp_console_cmd_t HF_ORDER(rc) = {
            .command = "rc",
            .help = hf_cmd_explain[HF_CMD_IDX_RC],
            .hint = NULL,
            .func = hf_cmd_tbl[HF_CMD_IDX_RC].handler,
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&HF_ORDER(rc)));

        const esp_console_cmd_t HF_ORDER(dn) = {
            .command = "dn",
            .help = hf_cmd_explain[HF_CMD_IDX_DN],
            .hint = "<num>",
            .func = hf_cmd_tbl[HF_CMD_IDX_DN].handler,
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&HF_ORDER(dn)));

        const esp_console_cmd_t HF_ORDER(vron) = {
            .command = "vron",
            .help = hf_cmd_explain[HF_CMD_IDX_VRON],
            .hint = NULL,
            .func = hf_cmd_tbl[HF_CMD_IDX_VRON].handler,
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&HF_ORDER(vron)));

        const esp_console_cmd_t HF_ORDER(vroff) = {
            .command = "vroff",
            .help = hf_cmd_explain[HF_CMD_IDX_VROFF],
            .hint = NULL,
            .func = hf_cmd_tbl[HF_CMD_IDX_VROFF].handler,
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&HF_ORDER(vroff)));

        vu_args.tgt = arg_str1(NULL, NULL, "<tgt>", "\n        0-speaker\n        1-microphone");
        vu_args.vol = arg_str1(NULL, NULL, "<vol>", "volume gain ranges from 0 to 15");
        vu_args.end = arg_end(1);
        const esp_console_cmd_t HF_ORDER(vu) = {
            .command = "vu",
            .help = hf_cmd_explain[HF_CMD_IDX_VU],
            .hint = NULL,
            .func = hf_cmd_tbl[HF_CMD_IDX_VU].handler,
            .argtable = &vu_args
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&HF_ORDER(vu)));

        const esp_console_cmd_t HF_ORDER(end) = {
            .command = "end",
            .help = hf_cmd_explain[HF_CMD_IDX_END],
            .hint = NULL,
            .func = hf_cmd_tbl[HF_CMD_IDX_END].handler,
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&HF_ORDER(end)));

        const esp_console_cmd_t HF_ORDER(iron) = {
            .command = "iron",
            .help = hf_cmd_explain[HF_CMD_IDX_IRON],
            .hint = NULL,
            .func = hf_cmd_tbl[HF_CMD_IDX_IRON].handler,
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&HF_ORDER(iron)));

        const esp_console_cmd_t HF_ORDER(iroff) = {
            .command = "iroff",
            .help = hf_cmd_explain[HF_CMD_IDX_IROFF],
            .hint = NULL,
            .func = hf_cmd_tbl[HF_CMD_IDX_IROFF].handler,
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&HF_ORDER(iroff)));

        ind_args.ind_type = arg_str1(NULL, NULL, "<ind_type>", "\n    1-call\n    2-callsetup\n    3-serval\n \
   4-signal\n    5-roam\n    6-battery\n    7-callheld");
        ind_args.value = arg_str1(NULL, NULL, "<value>", "value of indicator type");
        ind_args.end = arg_end(1);
        const esp_console_cmd_t HF_ORDER(ciev) = {
            .command = "ciev",
            .help = hf_cmd_explain[HF_CMD_IDX_CIEV],
            .hint = NULL,
            .func = hf_cmd_tbl[HF_CMD_IDX_CIEV].handler,
            .argtable = &ind_args
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&HF_ORDER(ciev)));

        ate_args.err = arg_str1(NULL, NULL, "<err>", "error code from 0 to 32");
        ate_args.rep = arg_str1(NULL, NULL, "<rep>", "response code from 0 to 7");
        ate_args.end = arg_end(1);
        const esp_console_cmd_t HF_ORDER(ate) = {
            .command = "ate",
            .help = hf_cmd_explain[HF_CMD_IDX_ATE],
            .hint = NULL,
            .func = hf_cmd_tbl[HF_CMD_IDX_ATE].handler,
            .argtable = &ate_args
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&HF_ORDER(ate)));

        const esp_console_cmd_t HF_ORDER(miclevel) = {
            .command = "miclevel",
            .help = hf_cmd_explain[HF_CMD_IDX_MICLEVEL],
            .hint = NULL,
            .func = hf_cmd_tbl[HF_CMD_IDX_MICLEVEL].handler,
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&HF_ORDER(miclevel)));

        const esp_console_cmd_t HF_ORDER(audiostatus) = {
            .command = "audiostatus",
            .help = hf_cmd_explain[HF_CMD_IDX_AUDIOSTATUS],
            .hint = NULL,
            .func = hf_cmd_tbl[HF_CMD_IDX_AUDIOSTATUS].handler,
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&HF_ORDER(audiostatus)));

        const esp_console_cmd_t HF_ORDER(call_start) = {
            .command = "call_start",
            .help = hf_cmd_explain[HF_CMD_IDX_CALL_START],
            .hint = NULL,
            .func = hf_cmd_tbl[HF_CMD_IDX_CALL_START].handler,
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&HF_ORDER(call_start)));

        const esp_console_cmd_t HF_ORDER(call_answer) = {
            .command = "call_answer",
            .help = hf_cmd_explain[HF_CMD_IDX_CALL_ANSWER],
            .hint = NULL,
            .func = hf_cmd_tbl[HF_CMD_IDX_CALL_ANSWER].handler,
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&HF_ORDER(call_answer)));

        const esp_console_cmd_t HF_ORDER(call_end) = {
            .command = "call_end",
            .help = hf_cmd_explain[HF_CMD_IDX_CALL_END],
            .hint = NULL,
            .func = hf_cmd_tbl[HF_CMD_IDX_CALL_END].handler,
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&HF_ORDER(call_end)));

        const esp_console_cmd_t HF_ORDER(force_audio) = {
            .command = "force_audio",
            .help = "Force audio data request and trigger incoming audio callback",
            .hint = NULL,
            .func = hf_cmd_tbl[HF_CMD_IDX_FORCE_AUDIO].handler,
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&HF_ORDER(force_audio)));

        const esp_console_cmd_t HF_ORDER(stop_mic) = {
            .command = "stop_mic",
            .help = "Stop microphone level monitoring",
            .hint = NULL,
            .func = hf_cmd_tbl[HF_CMD_IDX_STOP_MIC].handler,
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&HF_ORDER(stop_mic)));

        const esp_console_cmd_t HF_ORDER(stream_init) = {
            .command = "stream_init",
            .help = hf_cmd_explain[HF_CMD_IDX_STREAM_INIT],
            .hint = "<server_ip> <port>",
            .func = hf_cmd_tbl[HF_CMD_IDX_STREAM_INIT].handler,
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&HF_ORDER(stream_init)));

        const esp_console_cmd_t HF_ORDER(stream_start) = {
            .command = "stream_start",
            .help = hf_cmd_explain[HF_CMD_IDX_STREAM_START],
            .hint = NULL,
            .func = hf_cmd_tbl[HF_CMD_IDX_STREAM_START].handler,
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&HF_ORDER(stream_start)));

        const esp_console_cmd_t HF_ORDER(stream_stop) = {
            .command = "stream_stop",
            .help = hf_cmd_explain[HF_CMD_IDX_STREAM_STOP],
            .hint = NULL,
            .func = hf_cmd_tbl[HF_CMD_IDX_STREAM_STOP].handler,
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&HF_ORDER(stream_stop)));

        const esp_console_cmd_t HF_ORDER(stream_status) = {
            .command = "stream_status",
            .help = hf_cmd_explain[HF_CMD_IDX_STREAM_STATUS],
            .hint = NULL,
            .func = hf_cmd_tbl[HF_CMD_IDX_STREAM_STATUS].handler,
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&HF_ORDER(stream_status)));

        const esp_console_cmd_t HF_ORDER(wifi_connect) = {
            .command = "wifi_connect",
            .help = hf_cmd_explain[HF_CMD_IDX_WIFI_CONNECT],
            .hint = "<ssid> [password]",
            .func = hf_cmd_tbl[HF_CMD_IDX_WIFI_CONNECT].handler,
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&HF_ORDER(wifi_connect)));

        const esp_console_cmd_t HF_ORDER(wifi_disconnect) = {
            .command = "wifi_disconnect",
            .help = hf_cmd_explain[HF_CMD_IDX_WIFI_DISCONNECT],
            .hint = NULL,
            .func = hf_cmd_tbl[HF_CMD_IDX_WIFI_DISCONNECT].handler,
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&HF_ORDER(wifi_disconnect)));

        const esp_console_cmd_t HF_ORDER(wifi_status) = {
            .command = "wifi_status",
            .help = hf_cmd_explain[HF_CMD_IDX_WIFI_STATUS],
            .hint = NULL,
            .func = hf_cmd_tbl[HF_CMD_IDX_WIFI_STATUS].handler,
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&HF_ORDER(wifi_status)));

        const esp_console_cmd_t HF_ORDER(autostart_set) = {
            .command = "autostart_set",
            .help = hf_cmd_explain[HF_CMD_IDX_AUTOSTART_SET],
            .hint = "<command1> [command2] [...]",
            .func = hf_cmd_tbl[HF_CMD_IDX_AUTOSTART_SET].handler,
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&HF_ORDER(autostart_set)));

        const esp_console_cmd_t HF_ORDER(autostart_load_default) = {
            .command = "autostart_load_default",
            .help = hf_cmd_explain[HF_CMD_IDX_AUTOSTART_LOAD_DEFAULT],
            .hint = NULL,
            .func = hf_cmd_tbl[HF_CMD_IDX_AUTOSTART_LOAD_DEFAULT].handler,
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&HF_ORDER(autostart_load_default)));

        const esp_console_cmd_t HF_ORDER(autostart_show) = {
            .command = "autostart_show",
            .help = hf_cmd_explain[HF_CMD_IDX_AUTOSTART_SHOW],
            .hint = NULL,
            .func = hf_cmd_tbl[HF_CMD_IDX_AUTOSTART_SHOW].handler,
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&HF_ORDER(autostart_show)));

        const esp_console_cmd_t HF_ORDER(autostart_enable) = {
            .command = "autostart_enable",
            .help = hf_cmd_explain[HF_CMD_IDX_AUTOSTART_ENABLE],
            .hint = "<0|1>",
            .func = hf_cmd_tbl[HF_CMD_IDX_AUTOSTART_ENABLE].handler,
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&HF_ORDER(autostart_enable)));

        const esp_console_cmd_t HF_ORDER(autostart_clear) = {
            .command = "autostart_clear",
            .help = hf_cmd_explain[HF_CMD_IDX_AUTOSTART_CLEAR],
            .hint = NULL,
            .func = hf_cmd_tbl[HF_CMD_IDX_AUTOSTART_CLEAR].handler,
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&HF_ORDER(autostart_clear)));

        const esp_console_cmd_t HF_ORDER(autostart_run) = {
            .command = "autostart_run",
            .help = hf_cmd_explain[HF_CMD_IDX_AUTOSTART_RUN],
            .hint = NULL,
            .func = hf_cmd_tbl[HF_CMD_IDX_AUTOSTART_RUN].handler,
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&HF_ORDER(autostart_run)));
}
