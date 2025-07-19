#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_hf_ag_api.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bt_app_hf.h"
#include "app_hf_msg_set.h"
#include "call_simulation.h"

static const char *TAG = "CALL_SIM";
static bool s_call_active = false;
static bool s_mic_monitoring_active = false;
static TaskHandle_t s_mic_monitor_task_handle = NULL;

// Глобальная переменная для отслеживания состояния аудио (устанавливается в app_hf_msg_set.c)
extern bool g_audio_connected;
extern bool g_audio_connecting;

// Задача для автоматического ответа на звонок
static void auto_answer_task(void *param)
{
    // Ждем 2 секунды
    vTaskDelay(pdMS_TO_TICKS(2000));

    ESP_LOGI(TAG, "Auto-answering simulated call");
    answer_simulated_call();

    // Удаляем задачу после выполнения
    vTaskDelete(NULL);
}

// Задача для мониторинга уровня микрофона
static void microphone_level_monitor_task(void *param)
{
    ESP_LOGI(TAG, "Microphone level monitoring started");

    int counter = 0;
    int audio_connection_attempts = 0;
    bool was_connected = false;
    int stable_connection_counter = 0;

    while (s_mic_monitoring_active) {
        // Проверяем есть ли аудиосоединение
        bool audio_state = g_audio_connected;

        // Логирование текущего состояния аудио каждые 5 секунд
        if (counter % 10 == 0) {
            ESP_LOGI(TAG, "🔍 Audio state: %s (attempts: %d/3, stable: %d)",
                    audio_state ? "CONNECTED" : "DISCONNECTED",
                    audio_connection_attempts, stable_connection_counter);
        }

        // Отслеживаем стабильность соединения
        if (audio_state) {
            stable_connection_counter++;
            if (!was_connected) {
                ESP_LOGI(TAG, "🎉 Audio connection established! Starting data monitoring...");
                was_connected = true;
                audio_connection_attempts = 0; // Сброс счетчика при успешном подключении
            }
        } else {
            if (was_connected) {
                ESP_LOGW(TAG, "⚠️ Audio connection lost after %d stable cycles", stable_connection_counter);
                was_connected = false;
            }
            stable_connection_counter = 0;
        }

        // Если аудио не подключено, пытаемся активировать голосовое распознавание
        // чтобы инициировать аудиопоток
        if (!audio_state) {
            // Более консервативная стратегия - только если соединение совсем не работает
            if (counter % 15 == 0 && audio_connection_attempts < 3) {  // Каждые 7.5 секунд, максимум 3 раза
                ESP_LOGI(TAG, "🔄 Attempting audio connection... (attempt %d/3)",
                        audio_connection_attempts + 1);

                // Используем только прямое аудиосоединение, избегаем VRA которое вызывает конфликты
                ESP_LOGI(TAG, "🔗 Direct audio connection request...");
                esp_hf_ag_audio_connect(hf_peer_addr);
                audio_connection_attempts++;
            }

            if (audio_connection_attempts >= 3 && counter % 60 == 0) {
                ESP_LOGW(TAG, "❌ Unable to establish stable audio connection after 3 attempts.");
                ESP_LOGW(TAG, "💡 Possible solutions:");
                ESP_LOGW(TAG, "   1. Make sure your device supports HFP audio (headphones/car)");
                ESP_LOGW(TAG, "   2. Check device Bluetooth codec settings");
                ESP_LOGW(TAG, "   3. Try 'disa' then 'miclevel' again");
                ESP_LOGW(TAG, "   4. Some devices need manual audio activation");
            }
        } else {
            // Показываем статус когда аудио подключено
            if (counter % 20 == 0 && stable_connection_counter > 3) {
                ESP_LOGI(TAG, "🎤 Audio stream active - microphone data should be flowing...");
                ESP_LOGI(TAG, "📊 Watch for 'INCOMING AUDIO DATA' messages in BT_APP_HF logs");
            }
        }

        counter++;
        vTaskDelay(pdMS_TO_TICKS(500));  // проверка каждые 500 мс
    }

    ESP_LOGI(TAG, "Microphone level monitoring stopped");
    s_mic_monitor_task_handle = NULL;
    vTaskDelete(NULL);
}

esp_err_t start_incoming_call_simulation(bool auto_answer)
{
    if (s_call_active) {
        ESP_LOGW(TAG, "Call simulation already active");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Starting incoming call simulation");

    // Сообщаем о входящем звонке
    esp_hf_ag_ciev_report(hf_peer_addr, ESP_HF_IND_TYPE_CALLSETUP, ESP_HF_CALL_SETUP_STATUS_INCOMING);

    if (auto_answer) {
        // Создаем задачу для автоматического ответа
        BaseType_t ret = xTaskCreate(auto_answer_task, "auto_answer", 2048, NULL, 5, NULL);
        if (ret != pdPASS) {
            ESP_LOGE(TAG, "Failed to create auto-answer task");
            return ESP_ERR_NO_MEM;
        }
    }

    // Устанавливаем максимальный уровень сигнала
    esp_hf_ag_ciev_report(hf_peer_addr, ESP_HF_IND_TYPE_SIGNAL, 5);

    return ESP_OK;
}

esp_err_t answer_simulated_call(void)
{
    if (s_call_active) {
        ESP_LOGW(TAG, "Call already active");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Answering simulated call");

    // Устанавливаем флаг звонка
    s_call_active = true;

    // Уведомление о том, что звонок установлен
    esp_hf_ag_ciev_report(hf_peer_addr, ESP_HF_IND_TYPE_CALLSETUP, ESP_HF_CALL_SETUP_STATUS_IDLE);
    esp_hf_ag_ciev_report(hf_peer_addr, ESP_HF_IND_TYPE_CALL, ESP_HF_CALL_STATUS_CALL_IN_PROGRESS);

    // Обеспечиваем соединение аудио если оно ещё не активно
    ESP_LOGI(TAG, "Ensuring audio connection is active");
    esp_hf_ag_audio_connect(hf_peer_addr);

    return ESP_OK;
}

esp_err_t end_simulated_call(void)
{
    if (!s_call_active) {
        ESP_LOGW(TAG, "No active call to end");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Ending simulated call");

    // Уведомление о завершении звонка
    esp_hf_ag_ciev_report(hf_peer_addr, ESP_HF_IND_TYPE_CALL, ESP_HF_CALL_STATUS_NO_CALLS);

    // Сбросим флаг активного звонка
    s_call_active = false;

    return ESP_OK;
}

esp_err_t start_microphone_level_monitoring(void)
{
    if (s_mic_monitoring_active) {
        ESP_LOGI(TAG, "Microphone level monitoring is already active");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Starting microphone level monitoring");

    // Устанавливаем максимальную громкость микрофона используя правильный API
    esp_hf_ag_volume_control(hf_peer_addr, ESP_HF_VOLUME_CONTROL_TARGET_MIC, 15);

    // НОВЫЙ ПОДХОД: Симулируем входящий звонок для принуждения к аудиосоединению
    ESP_LOGI(TAG, "🔄 Simulating incoming call to force audio connection...");

    // Симулируем входящий звонок
    esp_hf_ag_ciev_report(hf_peer_addr, ESP_HF_IND_TYPE_CALLSETUP, ESP_HF_CALL_SETUP_STATUS_INCOMING);

    // Небольшая задержка
    vTaskDelay(pdMS_TO_TICKS(100));

    // Автоматически отвечаем на звонок
    ESP_LOGI(TAG, "🔄 Auto-answering call to establish audio...");
    esp_hf_ag_ciev_report(hf_peer_addr, ESP_HF_IND_TYPE_CALLSETUP, ESP_HF_CALL_SETUP_STATUS_IDLE);
    esp_hf_ag_ciev_report(hf_peer_addr, ESP_HF_IND_TYPE_CALL, ESP_HF_CALL_STATUS_CALL_IN_PROGRESS);

    // Теперь запрашиваем аудио соединение
    ESP_LOGI(TAG, "🔗 Requesting audio connection during call...");
    esp_hf_ag_audio_connect(hf_peer_addr);

    // Устанавливаем флаг активного мониторинга
    s_mic_monitoring_active = true;

    // Создаем задачу для мониторинга состояния подключения
    BaseType_t ret = xTaskCreate(microphone_level_monitor_task, "mic_monitor",
                                 2048, NULL, 5, &s_mic_monitor_task_handle);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create microphone monitoring task");
        s_mic_monitoring_active = false;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Audio connection requested during simulated call.");
    ESP_LOGI(TAG, "📞 Call simulation active - this should force audio connection");
    ESP_LOGI(TAG, "Use 'disa' command to disconnect audio when done.");

    return ESP_OK;
}

esp_err_t stop_microphone_level_monitoring(void)
{
    if (!s_mic_monitoring_active) {
        ESP_LOGW(TAG, "Microphone level monitoring is not active");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Stopping microphone level monitoring");

    // Сбрасываем флаг мониторинга ПЕРВЫМ, чтобы задача могла завершиться
    s_mic_monitoring_active = false;

    // Даем время задаче завершиться
    vTaskDelay(pdMS_TO_TICKS(100));

    // Задача сама завершится при следующей итерации цикла
    // Ждем еще немного для гарантии
    if (s_mic_monitor_task_handle != NULL) {
        // Ждем завершения задачи (максимум 500ms)
        int wait_count = 0;
        while (s_mic_monitor_task_handle != NULL && wait_count < 10) {
            vTaskDelay(pdMS_TO_TICKS(50));
            wait_count++;
        }

        if (s_mic_monitor_task_handle != NULL) {
            ESP_LOGW(TAG, "Microphone monitoring task did not finish gracefully, may have been deleted externally");
        } else {
            ESP_LOGI(TAG, "Microphone monitoring task finished successfully");
        }
    }

    return ESP_OK;
}

bool is_microphone_monitoring_active(void)
{
    return s_mic_monitoring_active;
}
