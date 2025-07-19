/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include "audio_streaming.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include <string.h>
#include <errno.h>
#include <inttypes.h>

static const char *TAG = "AUDIO_STREAM";

typedef struct {
    uint8_t *data;
    uint32_t size;
} audio_data_t;

static struct {
    audio_stream_config_t config;
    int socket_fd;
    bool is_running;
    bool is_connected;
    QueueHandle_t audio_queue;
    TaskHandle_t stream_task_handle;
    SemaphoreHandle_t mutex;
} s_audio_stream = {0};

static void audio_streaming_task(void *pvParameters);
static esp_err_t connect_to_server(void);
static void disconnect_from_server(void);

esp_err_t audio_streaming_init(const audio_stream_config_t *config)
{
    if (!config) {
        ESP_LOGE(TAG, "Config is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    // Копируем конфигурацию
    memcpy(&s_audio_stream.config, config, sizeof(audio_stream_config_t));

    // Создаем очередь для аудиоданных (увеличиваем размер)
    s_audio_stream.audio_queue = xQueueCreate(50, sizeof(audio_data_t));
    if (!s_audio_stream.audio_queue) {
        ESP_LOGE(TAG, "Failed to create audio queue");
        return ESP_ERR_NO_MEM;
    }

    // Создаем мьютекс
    s_audio_stream.mutex = xSemaphoreCreateMutex();
    if (!s_audio_stream.mutex) {
        ESP_LOGE(TAG, "Failed to create mutex");
        vQueueDelete(s_audio_stream.audio_queue);
        return ESP_ERR_NO_MEM;
    }

    s_audio_stream.socket_fd = -1;
    s_audio_stream.is_running = false;
    s_audio_stream.is_connected = false;

    ESP_LOGI(TAG, "Audio streaming initialized for server %s:%d",
             config->server_ip, config->server_port);

    return ESP_OK;
}

esp_err_t audio_streaming_start(void)
{
    if (s_audio_stream.is_running) {
        ESP_LOGW(TAG, "Audio streaming already running");
        return ESP_OK;
    }

    s_audio_stream.is_running = true;

    // Создаем задачу для потоковой передачи
    BaseType_t ret = xTaskCreate(
        audio_streaming_task,
        "audio_stream_task",
        4096,
        NULL,
        5,
        &s_audio_stream.stream_task_handle
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create streaming task");
        s_audio_stream.is_running = false;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Audio streaming started");
    return ESP_OK;
}

esp_err_t audio_streaming_stop(void)
{
    if (!s_audio_stream.is_running) {
        return ESP_OK;
    }

    s_audio_stream.is_running = false;

    // Ждем завершения задачи
    if (s_audio_stream.stream_task_handle) {
        vTaskDelete(s_audio_stream.stream_task_handle);
        s_audio_stream.stream_task_handle = NULL;
    }

    disconnect_from_server();

    // Очищаем очередь
    audio_data_t data;
    while (xQueueReceive(s_audio_stream.audio_queue, &data, 0) == pdTRUE) {
        free(data.data);
    }

    ESP_LOGI(TAG, "Audio streaming stopped");
    return ESP_OK;
}

esp_err_t audio_streaming_send(const uint8_t *data, uint32_t size)
{
    if (!s_audio_stream.is_running || !data || size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    // Проверяем размер очереди и освобождаем место при необходимости
    UBaseType_t queue_length = uxQueueMessagesWaiting(s_audio_stream.audio_queue);

    // Если очередь почти полная (более 80% заполнена), удаляем старые элементы
    if (queue_length > (50 * 8 / 10)) {  // 80% от максимального размера 50
        audio_data_t old_data;
        // Удаляем несколько старых элементов
        for (int i = 0; i < 5 && xQueueReceive(s_audio_stream.audio_queue, &old_data, 0) == pdTRUE; i++) {
            free(old_data.data);
        }
        ESP_LOGD(TAG, "Queue cleanup: removed old packets, queue size: %d",
                 (int)uxQueueMessagesWaiting(s_audio_stream.audio_queue));
    }

    // Создаем копию данных
    uint8_t *data_copy = malloc(size);
    if (!data_copy) {
        ESP_LOGW(TAG, "Failed to allocate memory for audio data");
        return ESP_ERR_NO_MEM;
    }

    memcpy(data_copy, data, size);

    audio_data_t audio_data = {
        .data = data_copy,
        .size = size
    };

    // Отправляем в очередь (неблокирующий вызов)
    if (xQueueSend(s_audio_stream.audio_queue, &audio_data, 0) != pdTRUE) {
        // Очередь переполнена, освобождаем память
        free(data_copy);
        ESP_LOGW(TAG, "Audio queue full, dropping packet");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

bool audio_streaming_is_connected(void)
{
    return s_audio_stream.is_connected;
}

esp_err_t audio_streaming_deinit(void)
{
    audio_streaming_stop();

    if (s_audio_stream.audio_queue) {
        vQueueDelete(s_audio_stream.audio_queue);
        s_audio_stream.audio_queue = NULL;
    }

    if (s_audio_stream.mutex) {
        vSemaphoreDelete(s_audio_stream.mutex);
        s_audio_stream.mutex = NULL;
    }

    ESP_LOGI(TAG, "Audio streaming deinitialized");
    return ESP_OK;
}

static void audio_streaming_task(void *pvParameters)
{
    audio_data_t audio_data;

    ESP_LOGI(TAG, "Audio streaming task started");

    while (s_audio_stream.is_running) {
        // Проверяем соединение
        if (!s_audio_stream.is_connected) {
            ESP_LOGI(TAG, "Attempting to connect to server...");
            if (connect_to_server() == ESP_OK) {
                ESP_LOGI(TAG, "Connected to audio server");
            } else {
                ESP_LOGW(TAG, "Failed to connect, retrying in 5 seconds");
                vTaskDelay(pdMS_TO_TICKS(5000));
                continue;
            }
        }

        // Получаем данные из очереди
        if (xQueueReceive(s_audio_stream.audio_queue, &audio_data, pdMS_TO_TICKS(1000)) == pdTRUE) {
            // Отправляем данные на сервер
            int sent = send(s_audio_stream.socket_fd, audio_data.data, audio_data.size, 0);

            if (sent < 0) {
                ESP_LOGW(TAG, "Failed to send audio data: %s", strerror(errno));
                disconnect_from_server();
            } else if (sent != (int)audio_data.size) {
                ESP_LOGW(TAG, "Partial send: %d/%"PRIu32" bytes", sent, audio_data.size);
            }

            // Освобождаем память
            free(audio_data.data);
        }
    }

    disconnect_from_server();
    ESP_LOGI(TAG, "Audio streaming task finished");
    vTaskDelete(NULL);
}

static esp_err_t connect_to_server(void)
{
    struct sockaddr_in server_addr;

    // Создаем сокет
    s_audio_stream.socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (s_audio_stream.socket_fd < 0) {
        ESP_LOGE(TAG, "Failed to create socket: %s", strerror(errno));
        return ESP_FAIL;
    }

    // Настраиваем адрес сервера
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(s_audio_stream.config.server_port);

    if (inet_pton(AF_INET, s_audio_stream.config.server_ip, &server_addr.sin_addr) <= 0) {
        ESP_LOGE(TAG, "Invalid server IP address: %s", s_audio_stream.config.server_ip);
        close(s_audio_stream.socket_fd);
        s_audio_stream.socket_fd = -1;
        return ESP_FAIL;
    }

    // Подключаемся к серверу
    if (connect(s_audio_stream.socket_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGW(TAG, "Failed to connect to server: %s", strerror(errno));
        close(s_audio_stream.socket_fd);
        s_audio_stream.socket_fd = -1;
        return ESP_FAIL;
    }

    // Отправляем заголовок с информацией о потоке
    char header[256];
    int header_len = snprintf(header, sizeof(header),
        "AUDIO_STREAM\n"
        "sample_rate=%"PRIu32"\n"
        "channels=%"PRIu8"\n"
        "bits_per_sample=%"PRIu8"\n"
        "codec=%s\n"
        "\n",
        s_audio_stream.config.sample_rate,
        s_audio_stream.config.channels,
        s_audio_stream.config.bits_per_sample,
        (s_audio_stream.config.sample_rate == 16000) ? "MSBC" : "CVSD");

    if (send(s_audio_stream.socket_fd, header, header_len, 0) < 0) {
        ESP_LOGE(TAG, "Failed to send header");
        close(s_audio_stream.socket_fd);
        s_audio_stream.socket_fd = -1;
        return ESP_FAIL;
    }

    s_audio_stream.is_connected = true;
    return ESP_OK;
}

static void disconnect_from_server(void)
{
    if (s_audio_stream.socket_fd >= 0) {
        close(s_audio_stream.socket_fd);
        s_audio_stream.socket_fd = -1;
    }
    s_audio_stream.is_connected = false;
}
