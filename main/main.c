/*
 * SPDX-FileCopyrightText: 2021-2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_bt.h"
#include "bt_app_core.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_bt_api.h"
#include "esp_hf_ag_api.h"
#include "bt_app_hf.h"
#include "gpio_pcm_config.h"
#include "esp_console.h"
#include "app_hf_msg_set.h"
#include "wifi_manager.h"
#include "autostart.h"

#define BT_HF_AG_TAG    "HF_AG_DEMO_MAIN"

/* event for handler "hf_ag_hdl_stack_up */
enum {
    BT_APP_EVT_STACK_UP = 0,
};

/* handler for bluetooth stack enabled events */
static void bt_hf_hdl_stack_evt(uint16_t event, void *p_param)
{
    ESP_LOGD(BT_HF_TAG, "%s evt %d", __func__, event);
    switch (event)
    {
        case BT_APP_EVT_STACK_UP:
        {
            /* set up device name */
            char *dev_name = "ESP_HFP_AG";
            esp_bt_dev_set_device_name(dev_name);

            esp_hf_ag_register_callback(bt_app_hf_cb);

            // init and register for HFP_AG functions
            esp_hf_ag_init();

            // Настройка SCO для максимальной совместимости с гарнитурами
            // Принудительно используем базовый SCO вместо eSCO для старых устройств
            esp_bt_cod_t cod = {
                .major = ESP_BT_COD_MAJOR_DEV_PHONE,
                .minor = 0x0C, // Значение для сотового телефона
                .service = 0
            };
            esp_bt_gap_set_cod(cod, ESP_BT_SET_COD_SERVICE_CLASS);

            ESP_LOGI(BT_HF_TAG, "🔧 HF AG initialized with basic SCO compatibility mode");

            /*
            * Set default parameters for Legacy Pairing
            * Use variable pin, input pin code when pairing
            */
            esp_bt_pin_type_t pin_type = ESP_BT_PIN_TYPE_VARIABLE;
            esp_bt_pin_code_t pin_code;
            pin_code[0] = '0';
            pin_code[1] = '0';
            pin_code[2] = '0';
            pin_code[3] = '0';
            esp_bt_gap_set_pin(pin_type, 4, pin_code);

            /* set discoverable and connectable mode, wait to be connected */
            esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
            break;
        }
        default:
            ESP_LOGE(BT_HF_TAG, "%s unhandled evt %d", __func__, event);
            break;
    }
}

void app_main(void)
{
    /* Initialize NVS — it is used to store PHY calibration data */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_BLE));

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    if ((ret = esp_bt_controller_init(&bt_cfg)) != ESP_OK) {
        ESP_LOGE(BT_HF_TAG, "%s initialize controller failed: %s", __func__, esp_err_to_name(ret));
        return;
    }
    if ((ret = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT)) != ESP_OK) {
        ESP_LOGE(BT_HF_TAG, "%s enable controller failed: %s", __func__, esp_err_to_name(ret));
        return;
    }
    esp_bluedroid_config_t bluedroid_cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    if ((ret = esp_bluedroid_init_with_cfg(&bluedroid_cfg)) != ESP_OK) {
        ESP_LOGE(BT_HF_TAG, "%s initialize bluedroid failed: %s", __func__, esp_err_to_name(ret));
        return;
    }
    if ((ret = esp_bluedroid_enable()) != ESP_OK) {
        ESP_LOGE(BT_HF_TAG, "%s enable bluedroid failed: %s", __func__, esp_err_to_name(ret));
        return;
    }

    /* create application task */
    bt_app_task_start_up();

    /* Bluetooth device name, connection mode and profile set up */
    bt_app_work_dispatch(bt_hf_hdl_stack_evt, BT_APP_EVT_STACK_UP, NULL, 0, NULL);

    /* Initialize WiFi Manager for audio streaming */
    ESP_LOGI(BT_HF_AG_TAG, "Initializing WiFi Manager...");
    ret = wifi_manager_init();
    if (ret != ESP_OK) {
        ESP_LOGE(BT_HF_AG_TAG, "Failed to initialize WiFi Manager: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(BT_HF_AG_TAG, "✅ WiFi Manager initialized - use 'wifi_connect' command to connect");
    }

#if CONFIG_BT_HFP_AUDIO_DATA_PATH_PCM
    /* configure the PCM interface and PINs used */
    app_gpio_pcm_io_cfg();
#endif

    /* configure externel chip for acoustic echo cancellation */
#if ACOUSTIC_ECHO_CANCELLATION_ENABLE
    app_gpio_aec_io_cfg();
#endif /* ACOUSTIC_ECHO_CANCELLATION_ENABLE */

    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    esp_console_dev_uart_config_t uart_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    repl_config.prompt = "hfp_ag>";

    // init console REPL environment
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&uart_config, &repl_config, &repl));

    /* Register commands */
    register_hfp_ag();

    /* Initialize autostart system */
    ESP_LOGI(BT_HF_AG_TAG, "Initializing autostart system...");
    ret = autostart_init();
    if (ret != ESP_OK) {
        ESP_LOGE(BT_HF_AG_TAG, "Failed to initialize autostart: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(BT_HF_AG_TAG, "✅ Autostart system initialized");

        // Добавляем задержку для инициализации всех компонентов
        vTaskDelay(pdMS_TO_TICKS(2000));

        // Выполняем автозапуск команд
        ESP_LOGI(BT_HF_AG_TAG, "Executing autostart commands...");
        autostart_execute();
    }

    printf("\n ==================================================\n");
    printf(" |       Steps to test hfp_ag                     |\n");
    printf(" |                                                |\n");
    printf(" |  1. Print 'help' to gain overview of commands  |\n");
    printf(" |  2. Setup a service level connection           |\n");
    printf(" |  3. Run hfp_ag to test                         |\n");
    printf(" |                                                |\n");
    printf(" =================================================\n\n");

    // start console REPL
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}
