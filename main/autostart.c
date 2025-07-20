#include "autostart.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_console.h"
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>  // Добавляем для PRIu32

static const char *TAG = "AUTOSTART";
static const char *NVS_NAMESPACE = "autostart";
static const char *NVS_KEY_ENABLED = "enabled";
static const char *NVS_KEY_COUNT = "cmd_count";
static const char *NVS_KEY_CMD_PREFIX = "cmd_";

static bool s_autostart_initialized = false;

esp_err_t autostart_init(void)
{
    if (s_autostart_initialized) {
        return ESP_OK;
    }

    // Инициализируем NVS если еще не инициализирован
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    s_autostart_initialized = true;
    ESP_LOGI(TAG, "Autostart system initialized");
    return ESP_OK;
}

esp_err_t autostart_execute(void)
{
    if (!autostart_is_enabled()) {
        ESP_LOGI(TAG, "Autostart is disabled");
        return ESP_OK;
    }

    char **commands = NULL;
    size_t count = 0;

    esp_err_t ret = autostart_load_commands(&commands, &count);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to load autostart commands: %s", esp_err_to_name(ret));
        return ret;
    }

    if (count == 0) {
        ESP_LOGI(TAG, "No autostart commands configured");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "🚀 Executing %zu autostart commands...", count);

    for (size_t i = 0; i < count; i++) {
        if (commands[i] && strlen(commands[i]) > 0) {
            ESP_LOGI(TAG, "📝 Command %zu: %s", i + 1, commands[i]);

            // Добавляем небольшую задержку между командами
            vTaskDelay(pdMS_TO_TICKS(500));

            // Выполняем команду через консоль ESP32
            int ret_cmd;
            esp_err_t err = esp_console_run(commands[i], &ret_cmd);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "⚠️ Command failed: %s (error: %s)",
                         commands[i], esp_err_to_name(err));
            } else if (ret_cmd != 0) {
                ESP_LOGW(TAG, "⚠️ Command returned error code: %d", ret_cmd);
            } else {
                ESP_LOGI(TAG, "✅ Command executed successfully");
            }
        }
    }

    // Освобождаем память
    for (size_t i = 0; i < count; i++) {
        free(commands[i]);
    }
    free(commands);

    ESP_LOGI(TAG, "🎯 Autostart execution completed");
    return ESP_OK;
}

esp_err_t autostart_save_commands(const char **commands, size_t count)
{
    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS handle: %s", esp_err_to_name(ret));
        return ret;
    }

    // Сохраняем количество команд
    ret = nvs_set_u32(nvs_handle, NVS_KEY_COUNT, count);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save command count: %s", esp_err_to_name(ret));
        nvs_close(nvs_handle);
        return ret;
    }

    // Сохраняем каждую команду
    for (size_t i = 0; i < count; i++) {
        char key[32];
        snprintf(key, sizeof(key), "%s%zu", NVS_KEY_CMD_PREFIX, i);

        ret = nvs_set_str(nvs_handle, key, commands[i]);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to save command %zu: %s", i, esp_err_to_name(ret));
            nvs_close(nvs_handle);
            return ret;
        }
    }

    ret = nvs_commit(nvs_handle);
    nvs_close(nvs_handle);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "💾 Saved %zu autostart commands", count);
    }

    return ret;
}

esp_err_t autostart_load_commands(char ***commands, size_t *count)
{
    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (ret != ESP_OK) {
        if (ret == ESP_ERR_NVS_NOT_FOUND) {
            *commands = NULL;
            *count = 0;
            return ESP_OK;
        }
        ESP_LOGE(TAG, "Failed to open NVS handle: %s", esp_err_to_name(ret));
        return ret;
    }

    // Загружаем количество команд
    uint32_t cmd_count = 0;
    ret = nvs_get_u32(nvs_handle, NVS_KEY_COUNT, &cmd_count);
    if (ret != ESP_OK) {
        if (ret == ESP_ERR_NVS_NOT_FOUND) {
            *commands = NULL;
            *count = 0;
            nvs_close(nvs_handle);
            return ESP_OK;
        }
        ESP_LOGE(TAG, "Failed to load command count: %s", esp_err_to_name(ret));
        nvs_close(nvs_handle);
        return ret;
    }

    if (cmd_count == 0) {
        *commands = NULL;
        *count = 0;
        nvs_close(nvs_handle);
        return ESP_OK;
    }

    // Выделяем память для массива указателей
    char **cmd_array = malloc(cmd_count * sizeof(char*));
    if (!cmd_array) {
        ESP_LOGE(TAG, "Failed to allocate memory for commands array");
        nvs_close(nvs_handle);
        return ESP_ERR_NO_MEM;
    }

    // Загружаем каждую команду
    for (uint32_t i = 0; i < cmd_count; i++) {
        char key[32];
        snprintf(key, sizeof(key), "%s%"PRIu32, NVS_KEY_CMD_PREFIX, i);

        // Сначала получаем размер строки
        size_t str_size = 0;
        ret = nvs_get_str(nvs_handle, key, NULL, &str_size);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to get size for command %"PRIu32": %s", i, esp_err_to_name(ret));
            // Освобождаем уже выделенную память
            for (uint32_t j = 0; j < i; j++) {
                free(cmd_array[j]);
            }
            free(cmd_array);
            nvs_close(nvs_handle);
            return ret;
        }

        // Выделяем память и загружаем строку
        cmd_array[i] = malloc(str_size);
        if (!cmd_array[i]) {
            ESP_LOGE(TAG, "Failed to allocate memory for command %"PRIu32, i);
            for (uint32_t j = 0; j < i; j++) {
                free(cmd_array[j]);
            }
            free(cmd_array);
            nvs_close(nvs_handle);
            return ESP_ERR_NO_MEM;
        }

        ret = nvs_get_str(nvs_handle, key, cmd_array[i], &str_size);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to load command %"PRIu32": %s", i, esp_err_to_name(ret));
            for (uint32_t j = 0; j <= i; j++) {
                free(cmd_array[j]);
            }
            free(cmd_array);
            nvs_close(nvs_handle);
            return ret;
        }
    }

    nvs_close(nvs_handle);

    *commands = cmd_array;
    *count = cmd_count;

    ESP_LOGI(TAG, "📂 Loaded %"PRIu32" autostart commands", cmd_count);
    return ESP_OK;
}

esp_err_t autostart_clear(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS handle: %s", esp_err_to_name(ret));
        return ret;
    }

    // Удаляем все ключи в namespace
    ret = nvs_erase_all(nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to clear autostart data: %s", esp_err_to_name(ret));
        nvs_close(nvs_handle);
        return ret;
    }

    ret = nvs_commit(nvs_handle);
    nvs_close(nvs_handle);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "🗑️ Autostart commands cleared");
    }

    return ret;
}

esp_err_t autostart_set_enabled(bool enable)
{
    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS handle: %s", esp_err_to_name(ret));
        return ret;
    }

    uint8_t enabled = enable ? 1 : 0;
    ret = nvs_set_u8(nvs_handle, NVS_KEY_ENABLED, enabled);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save enabled state: %s", esp_err_to_name(ret));
        nvs_close(nvs_handle);
        return ret;
    }

    ret = nvs_commit(nvs_handle);
    nvs_close(nvs_handle);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "🔧 Autostart %s", enable ? "enabled" : "disabled");
    }

    return ret;
}

bool autostart_is_enabled(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (ret != ESP_OK) {
        // По умолчанию включено, если настройки не найдены
        return true;
    }

    uint8_t enabled = 1; // по умолчанию включено
    ret = nvs_get_u8(nvs_handle, NVS_KEY_ENABLED, &enabled);
    nvs_close(nvs_handle);

    return enabled != 0;
}
