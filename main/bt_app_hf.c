/*
 * SPDX-FileCopyrightText: 2021-2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_bt_api.h"
#include "esp_hf_ag_api.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/ringbuf.h"
#include "time.h"
#include "sys/time.h"
#include "sdkconfig.h"
#include "bt_app_core.h"
#include "bt_app_hf.h"
#include "app_hf_msg_set.h"
#include "osi/allocator.h"
#include <math.h>
#include "call_simulation.h"
#include "audio_streaming.h"

const char *c_hf_evt_str[] = {
    "CONNECTION_STATE_EVT",              /*!< SERVICE LEVEL CONNECTION STATE CONTROL */
    "AUDIO_STATE_EVT",                   /*!< AUDIO CONNECTION STATE CONTROL */
    "VR_STATE_CHANGE_EVT",               /*!< VOICE RECOGNITION CHANGE */
    "VOLUME_CONTROL_EVT",                /*!< AUDIO VOLUME CONTROL */
    "UNKNOW_AT_CMD",                     /*!< UNKNOW AT COMMAND RECIEVED */
    "IND_UPDATE",                        /*!< INDICATION UPDATE */
    "CIND_RESPONSE_EVT",                 /*!< CALL & DEVICE INDICATION */
    "COPS_RESPONSE_EVT",                 /*!< CURRENT OPERATOR EVENT */
    "CLCC_RESPONSE_EVT",                 /*!< LIST OF CURRENT CALL EVENT */
    "CNUM_RESPONSE_EVT",                 /*!< SUBSCRIBER INFORTMATION OF CALL EVENT */
    "DTMF_RESPONSE_EVT",                 /*!< DTMF TRANSFER EVT */
    "NREC_RESPONSE_EVT",                 /*!< NREC RESPONSE EVT */
    "ANSWER_INCOMING_EVT",               /*!< ANSWER INCOMING EVT */
    "REJECT_INCOMING_EVT",               /*!< AREJECT INCOMING EVT */
    "DIAL_EVT",                          /*!< DIAL INCOMING EVT */
    "WBS_EVT",                           /*!< CURRENT CODEC EVT */
    "BCS_EVT",                           /*!< CODEC NEGO EVT */
    "PKT_STAT_EVT",                      /*!< REQUEST PACKET STATUS EVT */
};

//esp_hf_connection_state_t
const char *c_connection_state_str[] = {
    "DISCONNECTED",
    "CONNECTING",
    "CONNECTED",
    "SLC_CONNECTED",
    "DISCONNECTING",
};

// esp_hf_audio_state_t
const char *c_audio_state_str[] = {
    "disconnected",
    "connecting",
    "connected",
    "connected_msbc",
};

/// esp_hf_vr_state_t
const char *c_vr_state_str[] = {
    "Disabled",
    "Enabled",
};

// esp_hf_nrec_t
const char *c_nrec_status_str[] = {
    "NREC DISABLE",
    "NREC ABLE",
};

// esp_hf_control_target_t
const char *c_volume_control_target_str[] = {
    "SPEAKER",
    "MICROPHONE",
};

// esp_hf_subscriber_service_type_t
char *c_operator_name_str[] = {
    "China Mobile",
    "China Unicom",
    "China Telecom",
};

// esp_hf_subscriber_service_type_t
char *c_subscriber_service_type_str[] = {
    "UNKNOWN",
    "VOICE",
    "FAX",
};

// esp_hf_nego_codec_status_t
const char *c_codec_mode_str[] = {
    "CVSD Only",
    "Use CVSD",
    "Use MSBC",
};

#if CONFIG_BT_HFP_AUDIO_DATA_PATH_HCI
#define TABLE_SIZE         100
#define TABLE_SIZE_BYTE    200

#define ESP_HFP_RINGBUF_SIZE 3600

// 7500 microseconds(=12 slots) is aligned to 1 msbc frame duration, and is multiple of common Tesco for eSCO link with EV3 or 2-EV3 packet type
#define PCM_BLOCK_DURATION_US        (7500)

#define WBS_PCM_SAMPLING_RATE_KHZ    (16)
#define PCM_SAMPLING_RATE_KHZ        (8)

#define BYTES_PER_SAMPLE             (2)

// input can refer to Enhanced Setup Synchronous Connection Command in core spec4.2 Vol2, Part E
#define WBS_PCM_INPUT_DATA_SIZE  (WBS_PCM_SAMPLING_RATE_KHZ * PCM_BLOCK_DURATION_US / 1000 * BYTES_PER_SAMPLE) //240
#define PCM_INPUT_DATA_SIZE      (PCM_SAMPLING_RATE_KHZ * PCM_BLOCK_DURATION_US / 1000 * BYTES_PER_SAMPLE)     //120

#define PCM_GENERATOR_TICK_US        (4000)

static long s_data_num = 0;
static RingbufHandle_t s_m_rb = NULL;
static uint64_t s_time_new, s_time_old;
static esp_timer_handle_t s_periodic_timer;
static uint64_t s_last_enter_time, s_now_enter_time;
static uint64_t s_us_duration;
static SemaphoreHandle_t s_send_data_Semaphore = NULL;
static TaskHandle_t s_bt_app_send_data_task_handler = NULL;
static esp_hf_audio_state_t s_audio_code;

// --- Latency / sequence diagnostics ---
static uint32_t s_last_sent_seq = 0;
static uint32_t s_incoming_cb_counter = 0;      // counts every bt_app_hf_incoming_cb invocation
static uint64_t s_first_packet_time_us = 0;
static uint64_t s_last_log_time_us = 0;
static uint32_t s_lost_seq_estimate = 0;        // local detection of gaps (should normally stay 0)
static uint32_t s_prev_header_seq = 0;

// ---- Custom packet header for streaming (sequence + timestamp) ----
// This header is prepended (in little-endian format) before each audio payload
// sent to the server to help detect packet loss, reordering, and jitter.
// Server side must read this header (sizeof(stream_packet_header_t)) and then
// the raw audio payload that follows.
typedef struct __attribute__((packed)) {
    uint32_t magic;        // Magic marker to validate packet boundary (e.g. 0x41554448 'AUDH')
    uint32_t seq;          // Monotonic increasing sequence number
    uint64_t timestamp_us; // esp_timer_get_time() when packet was captured (microseconds)
    uint16_t payload_len;  // Length in bytes of the following audio payload
    uint16_t codec;        // 1 = CVSD, 2 = mSBC (extend as needed)
} stream_packet_header_t;

#define STREAM_PACKET_MAGIC 0x48445541u  // 'H''D''U''A' (new magic for updated protocol)
static uint32_t s_stream_seq = 0;
#define STREAM_CODEC_CVSD   1
#define STREAM_CODEC_MSBC   2

// Переменные для мониторинга уровня сигнала
static bool s_mic_level_monitoring = false;
static int s_mic_level_samples = 0;
static int64_t s_mic_level_sum = 0;
static int16_t s_mic_level_max = 0;
static esp_timer_handle_t s_mic_level_timer = NULL;

// Функция для анализа уровня сигнала
static void analyze_mic_level(const uint8_t *audio_buffer, uint32_t size) {
    if (!s_mic_level_monitoring) return;

    int16_t *samples = (int16_t *)audio_buffer;
    int num_samples = size / sizeof(int16_t);

    for (int i = 0; i < num_samples; i++) {
        int16_t sample = samples[i];
        int16_t abs_sample = abs(sample);

        s_mic_level_sum += abs_sample;
        s_mic_level_samples++;

        if (abs_sample > s_mic_level_max) {
            s_mic_level_max = abs_sample;
        }
    }

    // Дополнительное логирование для подтверждения поступления данных
    static int packet_count = 0;
    packet_count++;

    if (packet_count % 20 == 1) { // Каждые 20 пакетов
        ESP_LOGI(BT_HF_TAG, "🎤 AUDIO DATA RECEIVED: packet #%d, size=%"PRIu32", samples=%d, max_level=%d",
                 packet_count, size, num_samples, s_mic_level_max);
    }
}

// Callback для периодического вывода уровня сигнала
static void mic_level_report_timer_cb(void *arg) {
    if (!s_mic_level_monitoring || s_mic_level_samples == 0) return;

    int16_t average = s_mic_level_sum / s_mic_level_samples;
    float db_level = 20.0f * log10f((float)s_mic_level_max / 32767.0f);

    // Ограничиваем минимальное значение dB
    if (db_level < -60.0f) db_level = -60.0f;

    ESP_LOGI(BT_HF_TAG, "MIC LEVEL: avg=%d, max=%d, dB=%.1f, samples=%d",
             average, s_mic_level_max, db_level, s_mic_level_samples);

    // Сброс статистики
    s_mic_level_sum = 0;
    s_mic_level_samples = 0;
    s_mic_level_max = 0;
}

// Функции для управления мониторингом
void bt_app_start_mic_level_monitoring(void) {
    if (s_mic_level_monitoring) return;

    s_mic_level_monitoring = true;
    s_mic_level_sum = 0;
    s_mic_level_samples = 0;
    s_mic_level_max = 0;

    // Создаем таймер для периодического вывода (каждые 500ms)
    const esp_timer_create_args_t timer_args = {
        .callback = &mic_level_report_timer_cb,
        .name = "mic_level_timer"
    };

    if (s_mic_level_timer == NULL) {
        esp_timer_create(&timer_args, &s_mic_level_timer);
    }
    esp_timer_start_periodic(s_mic_level_timer, 500000); // 500ms

    ESP_LOGI(BT_HF_TAG, "Microphone level monitoring started");
}

void bt_app_stop_mic_level_monitoring(void) {
    if (!s_mic_level_monitoring) return;

    s_mic_level_monitoring = false;

    if (s_mic_level_timer) {
        esp_timer_stop(s_mic_level_timer);
    }

    ESP_LOGI(BT_HF_TAG, "Microphone level monitoring stopped");
}

static void print_speed(void);

static uint32_t bt_app_hf_outgoing_cb(uint8_t *p_buf, uint32_t sz)
{
    size_t item_size = 0;
    uint8_t *data;
    if (!s_m_rb) {
        return 0;
    }
    vRingbufferGetInfo(s_m_rb, NULL, NULL, NULL, NULL, &item_size);
    if (item_size >= sz) {
        data = xRingbufferReceiveUpTo(s_m_rb, &item_size, 0, sz);
        memcpy(p_buf, data, item_size);
        vRingbufferReturnItem(s_m_rb, data);
        return sz;
    } else {
        // data not enough, do not read\n
        return 0;
    }
    return 0;
}

static void bt_app_hf_incoming_cb(const uint8_t *buf, uint32_t sz)
{
    // Диагностика входящих PCM блоков (каждый вызов => 7.5 ms для CVSD / 120 байт)
    static uint32_t failed_sends = 0;
    s_incoming_cb_counter++;
    if (s_first_packet_time_us == 0) {
        s_first_packet_time_us = esp_timer_get_time();
        s_last_log_time_us = s_first_packet_time_us;
    }

    // Логируем ПЕРВЫЕ 10 пакетов для диагностики, затем каждый 200-й
    if (s_incoming_cb_counter <= 10 || s_incoming_cb_counter % 200 == 1) {
        ESP_LOGW(BT_HF_TAG, "🔥 INCOMING AUDIO CALLBACK #%"PRIu32": size=%"PRIu32" bytes, buf=%p",
                s_incoming_cb_counter, sz, buf);
    }

    if (sz == 0 || buf == NULL) {
        ESP_LOGW(BT_HF_TAG, "❌ Invalid incoming audio data: size=%"PRIu32", buf=%p", sz, buf);
        return;
    }

    // Принудительная диагностика первых байтов
    if (s_incoming_cb_counter <= 5) {
        ESP_LOGW(BT_HF_TAG, "📋 First 8 bytes: %02x %02x %02x %02x %02x %02x %02x %02x",
                buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7]);
    }

    if (s_incoming_cb_counter == 1) {
        ESP_LOGI(BT_HF_TAG, "Packet header size=%u bytes (magic=0x%08" PRIx32 ")", (unsigned)sizeof(stream_packet_header_t), (uint32_t)STREAM_PACKET_MAGIC);
    }

    s_time_new = esp_timer_get_time();
    s_data_num += sz;  // keep bandwidth stats

    // Анализ уровня сигнала микрофона
    analyze_mic_level(buf, sz);

    // Проверяем состояние аудио стриминга
    if (!audio_streaming_is_connected()) {
        if (s_incoming_cb_counter % 200 == 1) {
            ESP_LOGW(BT_HF_TAG, "⚠️ Audio streaming not connected! dropped=%"PRIu32, failed_sends);
        }
        failed_sends++;
        return;
    }

    // Формируем пакет с заголовком (sequence + timestamp) перед отправкой
    // Header + payload layout: [stream_packet_header_t][audio bytes...]
    uint32_t seq_to_send = s_stream_seq;  // capture current sequence for consistent local logging
    stream_packet_header_t header;
    header.magic        = STREAM_PACKET_MAGIC;
    header.seq          = s_stream_seq++;
    header.timestamp_us = esp_timer_get_time();
    header.payload_len  = (uint16_t)sz;
    header.codec        = (s_audio_code == ESP_HF_AUDIO_STATE_CONNECTED_MSBC) ? STREAM_CODEC_MSBC : STREAM_CODEC_CVSD;

    // Local gap detection (should normally be sequential: last + 1)
    if (s_prev_header_seq != 0 && seq_to_send != s_prev_header_seq + 1) {
        s_lost_seq_estimate += (seq_to_send - (s_prev_header_seq + 1));
    }
    s_prev_header_seq = seq_to_send;

    size_t packet_size = sizeof(header) + sz;
    uint8_t *packet = (uint8_t *)osi_malloc(packet_size);
    if (!packet) {
        failed_sends++;
        if (s_incoming_cb_counter % 200 == 1) {
            ESP_LOGE(BT_HF_TAG, "❌ OOM allocating packet (%u bytes), drops=%"PRIu32, (unsigned)packet_size, failed_sends);
        }
        return;
    }
    memcpy(packet, &header, sizeof(header));
    memcpy(packet + sizeof(header), buf, sz);

    esp_err_t stream_result = audio_streaming_send(packet, packet_size);
    osi_free(packet);

    if (stream_result != ESP_OK) {
        failed_sends++;
        if (s_incoming_cb_counter % 200 == 1) {
            ESP_LOGW(BT_HF_TAG, "📡 Audio streaming send failed: %s (total failed: %"PRIu32")",
                     esp_err_to_name(stream_result), failed_sends);
        }
    } else if (s_incoming_cb_counter % 400 == 1) {
        uint64_t now_us = esp_timer_get_time();
        uint64_t stream_duration_ms = (now_us - s_first_packet_time_us) / 1000;
        uint64_t packet_latency_us = now_us - header.timestamp_us;
        float pkt_rate = (float)s_incoming_cb_counter * 1000000.0f / (float)(now_us - s_first_packet_time_us); // packets per second
        ESP_LOGI(BT_HF_TAG,
                 "✅ TX pkt_cb=%"PRIu32" seq=%"PRIu32" sent payload=%"PRIu32"B latency=%"PRIu64"us rate=%.2fpps lost_local=%"PRIu32" uptime=%"PRIu64"ms",
                 s_incoming_cb_counter, header.seq, sz, packet_latency_us, pkt_rate, s_lost_seq_estimate, stream_duration_ms);
    }

    static bool latency_hint_logged = false;
    if (!latency_hint_logged) {
        ESP_LOGI(BT_HF_TAG, "ℹ️ Low-latency mode active: every PCM frame forwarded immediately with minimal buffering.");
        latency_hint_logged = true;
    }

    if ((s_time_new - s_time_old) >= 3000000) {
        print_speed();
    }
}

static uint32_t bt_app_hf_create_audio_data(uint8_t *p_buf, uint32_t sz)
{
    // Вместо синусоиды отправляем тишину (нули) или реальные данные
    // Это предотвратит воспроизведение искусственного звука в гарнитуре
    memset(p_buf, 0, sz);
    return sz;
}

static void print_speed(void)
{
    float tick_s = (s_time_new - s_time_old) / 1000000.0;
    float speed = s_data_num * 8 / tick_s / 1000.0;
    ESP_LOGI(BT_HF_TAG, "speed(%fs ~ %fs): %f kbit/s" , s_time_old / 1000000.0, s_time_new / 1000000.0, speed);
    s_data_num = 0;
    s_time_old = s_time_new;
}

static void bt_app_send_data_timer_cb(void *arg)
{
    // Проверяем существование семафора перед использованием
    if (s_send_data_Semaphore == NULL) {
        return;
    }

    // Используем неблокирующий вызов с таймаутом
    if (!xSemaphoreGive(s_send_data_Semaphore)) {
        // Логируем ошибку только изредка, чтобы не засорять лог
        static int error_count = 0;
        error_count++;
        if (error_count % 100 == 0) {
            ESP_LOGW(BT_HF_TAG, "Semaphore give failed occasionally (count: %d) - system overloaded", error_count);
        }
        return;
    }
    return;
}

static void bt_app_send_data_task(void *arg)
{
    uint64_t frame_data_num;
    size_t item_size = 0;
    uint8_t *buf = NULL;

    // Улучшенный контроль потока для предотвращения гапов
    static int send_counter = 0;
    static int consecutive_failures = 0;
    static uint64_t last_successful_send = 0;
    static uint64_t last_frame_time = 0;

    for (;;) {
        if (xSemaphoreTake(s_send_data_Semaphore, (TickType_t)portMAX_DELAY)) {
            send_counter++;

            // Проверяем состояние аудио соединения
            if (s_audio_code != ESP_HF_AUDIO_STATE_CONNECTED &&
                s_audio_code != ESP_HF_AUDIO_STATE_CONNECTED_MSBC) {
                continue;
            }

            s_now_enter_time = esp_timer_get_time();
            s_us_duration = s_now_enter_time - s_last_enter_time;

            // Стабилизируем тайминг - принудительно используем фиксированные интервалы
            uint64_t target_interval = PCM_BLOCK_DURATION_US; // 7500 мкс = 7.5мс
            if (last_frame_time != 0) {
                uint64_t time_since_last = s_now_enter_time - last_frame_time;
                if (time_since_last < target_interval) {
                    // Слишком рано - пропускаем этот кадр
                    continue;
                }
            }
            last_frame_time = s_now_enter_time;

            // Рассчитываем размер кадра более консервативно
            if(s_audio_code == ESP_HF_AUDIO_STATE_CONNECTED_MSBC) {
                frame_data_num = WBS_PCM_INPUT_DATA_SIZE; // Фиксированный размер
                s_last_enter_time = s_now_enter_time;
            } else {
                frame_data_num = PCM_INPUT_DATA_SIZE; // Фиксированный размер
                s_last_enter_time = s_now_enter_time;
            }

            // Снижаем приоритет исходящих данных при частых ошибках
            if (consecutive_failures > 3) {
                // Пропускаем каждый второй кадр при ошибках
                if (send_counter % 2 == 0) {
                    consecutive_failures = 0; // Сброс для предотвращения накопления
                    continue;
                }
            }

            buf = osi_malloc(frame_data_num);
            if (!buf) {
                ESP_LOGE(BT_HF_TAG, "%s, no mem", __FUNCTION__);
                consecutive_failures++;
                continue;
            }

            bt_app_hf_create_audio_data(buf, frame_data_num);

            // Используем очень короткий таймаут для ring buffer
            BaseType_t done = xRingbufferSend(s_m_rb, buf, frame_data_num, pdMS_TO_TICKS(1));
            if (!done) {
                consecutive_failures++;
                if (send_counter % 20 == 0) { // Реже логируем
                    ESP_LOGW(BT_HF_TAG, "rb send fail, consecutive failures: %d", consecutive_failures);
                }
            } else {
                consecutive_failures = 0;
                last_successful_send = esp_timer_get_time();
            }

            osi_free(buf);
            vRingbufferGetInfo(s_m_rb, NULL, NULL, NULL, NULL, &item_size);

            // Более консервативная проверка размера буфера
            size_t required_size = (s_audio_code == ESP_HF_AUDIO_STATE_CONNECTED_MSBC) ?
                                  WBS_PCM_INPUT_DATA_SIZE : PCM_INPUT_DATA_SIZE;

            // Отправляем данные только если буфер не переполнен
            if(item_size >= required_size && item_size < (required_size * 3)) {
                esp_hf_ag_outgoing_data_ready();
            }
        }
    }
}
void bt_app_send_data(void)
{
    s_send_data_Semaphore = xSemaphoreCreateBinary();
    xTaskCreate(bt_app_send_data_task, "BtAppSendDataTask", 6144, NULL, configMAX_PRIORITIES - 4, &s_bt_app_send_data_task_handler);
    s_m_rb = xRingbufferCreate(ESP_HFP_RINGBUF_SIZE, RINGBUF_TYPE_BYTEBUF);
    const esp_timer_create_args_t c_periodic_timer_args = {
            .callback = &bt_app_send_data_timer_cb,
            .name = "periodic"
    };
    ESP_ERROR_CHECK(esp_timer_create(&c_periodic_timer_args, &s_periodic_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_periodic_timer, PCM_GENERATOR_TICK_US)); // Возвращаем оригинальный интервал 4ms
    s_last_enter_time = esp_timer_get_time();

    ESP_LOGI(BT_HF_TAG, "✅ Audio send data task initialized with optimized low-latency settings");
    return;
}

void bt_app_send_data_shut_down(void)
{
    ESP_LOGI(BT_HF_TAG, "Shutting down audio data transmission...");

    s_stream_seq = 0; // Reset sequence for next session

    // Останавливаем мониторинг микрофона
    bt_app_stop_mic_level_monitoring();

    // КРИТИЧНО: Останавливаем таймер отправки данных ПЕРВЫМ, чтобы предотвратить новые события
    if(s_periodic_timer) {
        esp_err_t timer_stop_result = esp_timer_stop(s_periodic_timer);
        if (timer_stop_result == ESP_OK) {
            ESP_LOGI(BT_HF_TAG, "Periodic timer stopped successfully");
        } else {
            ESP_LOGW(BT_HF_TAG, "Timer stop returned: %s", esp_err_to_name(timer_stop_result));
        }

        esp_err_t timer_delete_result = esp_timer_delete(s_periodic_timer);
        if (timer_delete_result == ESP_OK) {
            ESP_LOGI(BT_HF_TAG, "Periodic timer deleted successfully");
        } else {
            ESP_LOGW(BT_HF_TAG, "Timer delete returned: %s", esp_err_to_name(timer_delete_result));
        }
        s_periodic_timer = NULL;
    }

    // Ждем завершения всех текущих операций с ring buffer
    vTaskDelay(pdMS_TO_TICKS(100));

    // Очищаем ring buffer перед удалением задачи
    if (s_m_rb) {
        // Извлекаем и отбрасываем все данные из буфера
        size_t item_size;
        void *item;
        while ((item = xRingbufferReceive(s_m_rb, &item_size, 0)) != NULL) {
            vRingbufferReturnItem(s_m_rb, item);
        }
        ESP_LOGI(BT_HF_TAG, "Ring buffer cleared");
    }

    // Разблокируем задачу отправки данных, если она ждёт семафор
    if (s_send_data_Semaphore) {
        // Даём семафор несколько раз для гарантированного разблокирования
        for (int i = 0; i < 10; i++) {
            xSemaphoreGive(s_send_data_Semaphore);
        }
        ESP_LOGI(BT_HF_TAG, "Semaphore signaled multiple times for task cleanup");
    }

    // Увеличиваем время ожидания для корректного завершения задачи
    vTaskDelay(pdMS_TO_TICKS(150));

    // Удаляем задачу отправки данных
    if (s_bt_app_send_data_task_handler) {
        vTaskDelete(s_bt_app_send_data_task_handler);
        s_bt_app_send_data_task_handler = NULL;
        ESP_LOGI(BT_HF_TAG, "Send data task deleted");
    }

    // Финальная пауза перед удалением ресурсов
    vTaskDelay(pdMS_TO_TICKS(50));

    // Удаляем семафор
    if (s_send_data_Semaphore) {
        vSemaphoreDelete(s_send_data_Semaphore);
        s_send_data_Semaphore = NULL;
        ESP_LOGI(BT_HF_TAG, "Semaphore deleted");
    }

    // Удаляем ring buffer
    if (s_m_rb) {
        vRingbufferDelete(s_m_rb);
        s_m_rb = NULL;
        ESP_LOGI(BT_HF_TAG, "Ring buffer deleted");
    }

    ESP_LOGI(BT_HF_TAG, "Audio data transmission shutdown complete - all resources cleaned up");
}
#endif /* #if CONFIG_BT_HFP_AUDIO_DATA_PATH_HCI */

void bt_app_hf_cb(esp_hf_cb_event_t event, esp_hf_cb_param_t *param)
{
    if (event <= ESP_HF_PKT_STAT_NUMS_GET_EVT) {
        ESP_LOGI(BT_HF_TAG, "APP HFP event: %s", c_hf_evt_str[event]);
    } else {
        ESP_LOGE(BT_HF_TAG, "APP HFP invalid event %d", event);
    }

    switch (event) {
        case ESP_HF_CONNECTION_STATE_EVT:
        {
            ESP_LOGI(BT_HF_TAG, "--connection state %s, peer feats 0x%"PRIx32", chld_feats 0x%"PRIx32,
                    c_connection_state_str[param->conn_stat.state],
                    param->conn_stat.peer_feat,
                    param->conn_stat.chld_feat);
            memcpy(hf_peer_addr, param->conn_stat.remote_bda, ESP_BD_ADDR_LEN);
            break;
        }

        case ESP_HF_AUDIO_STATE_EVT:
        {
            ESP_LOGI(BT_HF_TAG, "--Audio State %s", c_audio_state_str[param->audio_stat.state]);

            // Дополнительный лог для отслеживания событий AUDIO_STATE_EVT
            ESP_LOGI(BT_HF_TAG, "AUDIO_STATE_EVT: state=%d (CONNECTING=%d, CONNECTED=%d, CONNECTED_MSBC=%d, DISCONNECTED=%d)",
                     param->audio_stat.state,
                     ESP_HF_AUDIO_STATE_CONNECTING,
                     ESP_HF_AUDIO_STATE_CONNECTED,
                     ESP_HF_AUDIO_STATE_CONNECTED_MSBC,
                     ESP_HF_AUDIO_STATE_DISCONNECTED);

#if CONFIG_BT_HFP_AUDIO_DATA_PATH_HCI
            // Безопасное отключение аудио - ВАЖНО делать это ПЕРЕД обновлением состояния
            if (param->audio_stat.state == ESP_HF_AUDIO_STATE_DISCONNECTED) {
                ESP_LOGI(BT_HF_TAG, "--ESP AG Audio Connection Disconnected - cleaning up resources.");

                // Добавляем задержку для завершения текущих операций
                vTaskDelay(pdMS_TO_TICKS(100));

                // Безопасно останавливаем передачу данных
                ESP_LOGI(BT_HF_TAG, "Shutting down audio data transmission...");
                bt_app_send_data_shut_down();

                // Дополнительная задержка для стабилизации
                vTaskDelay(pdMS_TO_TICKS(200));

                ESP_LOGI(BT_HF_TAG, "Audio data transmission shutdown complete - all resources cleaned up");
            }
#endif

            // Update audio state tracking ПОСЛЕ безопасной обработки отключения
            if (param->audio_stat.state == ESP_HF_AUDIO_STATE_CONNECTING) {
                hf_audio_state_connecting();
            } else if (param->audio_stat.state == ESP_HF_AUDIO_STATE_CONNECTED ||
                       param->audio_stat.state == ESP_HF_AUDIO_STATE_CONNECTED_MSBC) {
                hf_audio_state_connected();
            } else if (param->audio_stat.state == ESP_HF_AUDIO_STATE_DISCONNECTED) {
                hf_audio_state_disconnected();
                // Останавливаем мониторинг микрофона при отключении
                bt_app_stop_mic_level_monitoring();
                ESP_LOGI(BT_HF_TAG, "Audio disconnected - stopping microphone level monitoring.");
            }

#if CONFIG_BT_HFP_AUDIO_DATA_PATH_HCI
            if (param->audio_stat.state == ESP_HF_AUDIO_STATE_CONNECTED ||
                param->audio_stat.state == ESP_HF_AUDIO_STATE_CONNECTED_MSBC)
            {
                ESP_LOGI(BT_HF_TAG, "🎉 Audio connection established! Setting up data callbacks...");

                if(param->audio_stat.state == ESP_HF_AUDIO_STATE_CONNECTED) {
                    s_audio_code = ESP_HF_AUDIO_STATE_CONNECTED;
                    ESP_LOGI(BT_HF_TAG, "Using CVSD codec");
                } else {
                    s_audio_code = ESP_HF_AUDIO_STATE_CONNECTED_MSBC;
                    ESP_LOGI(BT_HF_TAG, "Using mSBC codec (wideband)");
                }

                s_time_old = esp_timer_get_time();
                esp_hf_ag_register_data_callback(bt_app_hf_incoming_cb, bt_app_hf_outgoing_cb);

                /* Begin send esco data task */
                bt_app_send_data();
                ESP_LOGI(BT_HF_TAG, "✅ Audio data path initialized - ready to receive microphone data");

                // Принудительно активируем поток входящих данных
                ESP_LOGI(BT_HF_TAG, "🔄 Force enabling incoming audio data stream...");
                vTaskDelay(pdMS_TO_TICKS(100)); // Даем время для инициализации

                // Запрашиваем входящие данные немедленно
                esp_hf_ag_outgoing_data_ready();

                // Автоматически запускаем мониторинг микрофона, если включен мониторинг
                if (is_microphone_monitoring_active()) {
                    ESP_LOGI(BT_HF_TAG, "🎤 Starting microphone level monitoring автоматически.");
                    bt_app_start_mic_level_monitoring();

                    // Дополнительно активируем поток данных через небольшой интервал
                    vTaskDelay(pdMS_TO_TICKS(200));
                    ESP_LOGI(BT_HF_TAG, "🔄 Secondary data stream activation...");
                    esp_hf_ag_outgoing_data_ready();
                } else {
                    ESP_LOGI(BT_HF_TAG, "ℹ️  Audio connected but microphone monitoring is not active");
                    ESP_LOGI(BT_HF_TAG, "💡 Use 'miclevel' command to start monitoring microphone levels");
                }
            }
#endif /* #if CONFIG_BT_HFP_AUDIO_DATA_PATH_HCI */
            break;
        }

        case ESP_HF_BVRA_RESPONSE_EVT:
        {
            ESP_LOGI(BT_HF_TAG, "--Voice Recognition is %s", c_vr_state_str[param->vra_rep.value]);
            break;
        }

        case ESP_HF_VOLUME_CONTROL_EVT:
        {
            ESP_LOGI(BT_HF_TAG, "--Volume Target: %s, Volume %d", c_volume_control_target_str[param->volume_control.type], param->volume_control.volume);
            break;
        }

        case ESP_HF_UNAT_RESPONSE_EVT:
        {
            ESP_LOGI(BT_HF_TAG, "--UNKOW AT CMD: %s", param->unat_rep.unat);
            esp_hf_ag_unknown_at_send(param->unat_rep.remote_addr, NULL);
            break;
        }

        case ESP_HF_IND_UPDATE_EVT:
        {
            ESP_LOGI(BT_HF_TAG, "--UPDATE INDCATOR!");
            esp_hf_call_status_t call_state = 1;
            esp_hf_call_setup_status_t call_setup_state = 2;
            esp_hf_network_state_t ntk_state = 1;
            int signal = 2;
            int battery = 3;
            esp_hf_ag_ciev_report(param->ind_upd.remote_addr, ESP_HF_IND_TYPE_CALL, call_state);
            esp_hf_ag_ciev_report(param->ind_upd.remote_addr, ESP_HF_IND_TYPE_CALLSETUP, call_setup_state);
            esp_hf_ag_ciev_report(param->ind_upd.remote_addr, ESP_HF_IND_TYPE_SERVICE, ntk_state);
            esp_hf_ag_ciev_report(param->ind_upd.remote_addr, ESP_HF_IND_TYPE_SIGNAL, signal);
            esp_hf_ag_ciev_report(param->ind_upd.remote_addr, ESP_HF_IND_TYPE_BATTCHG, battery);
            break;
        }

        case ESP_HF_CIND_RESPONSE_EVT:
        {
            ESP_LOGI(BT_HF_TAG, "--CIND Start.");
            esp_hf_call_status_t call_status = 0;
            esp_hf_call_setup_status_t call_setup_status = 0;
            esp_hf_network_state_t ntk_state = 1;
            int signal = 4;
            esp_hf_roaming_status_t roam = 0;
            int batt_lev = 3;
            esp_hf_call_held_status_t call_held_status = 0;
            esp_hf_ag_cind_response(param->cind_rep.remote_addr,call_status,call_setup_status,ntk_state,signal,roam,batt_lev,call_held_status);
            break;
        }

        case ESP_HF_COPS_RESPONSE_EVT:
        {
            const int svc_type = 1;
            esp_hf_ag_cops_response(param->cops_rep.remote_addr, c_operator_name_str[svc_type]);
            break;
        }

        case ESP_HF_CLCC_RESPONSE_EVT:
        {
            int index = 1;
            //mandatory
            esp_hf_current_call_direction_t dir = 1;
            esp_hf_current_call_status_t current_call_status = 0;
            esp_hf_current_call_mode_t mode = 0;
            esp_hf_current_call_mpty_type_t mpty = 0;
            //option
            char *number = {"123456"};
            esp_hf_call_addr_type_t type = ESP_HF_CALL_ADDR_TYPE_UNKNOWN;

            ESP_LOGI(BT_HF_TAG, "--Calling Line Identification.");
            esp_hf_ag_clcc_response(param->clcc_rep.remote_addr, index, dir, current_call_status, mode, mpty, number, type);

            //AG shall always send ok response to HF
            //index = 0 means response ok
            index = 0;
            esp_hf_ag_clcc_response(param->clcc_rep.remote_addr, index, dir, current_call_status, mode, mpty, number, type);
            break;
        }

        case ESP_HF_CNUM_RESPONSE_EVT:
        {
            char *number = {"123456"};
            int number_type = 129;
            esp_hf_subscriber_service_type_t service_type = ESP_HF_SUBSCRIBER_SERVICE_TYPE_VOICE;
            if (service_type == ESP_HF_SUBSCRIBER_SERVICE_TYPE_VOICE || service_type == ESP_HF_SUBSCRIBER_SERVICE_TYPE_FAX) {
                ESP_LOGI(BT_HF_TAG, "--Current Number is %s, Number Type is %d, Service Type is %s.", number, number_type, c_subscriber_service_type_str[service_type - 3]);
            } else {
                ESP_LOGI(BT_HF_TAG, "--Current Number is %s, Number Type is %d, Service Type is %s.", number, number_type, c_subscriber_service_type_str[0]);
            }
            esp_hf_ag_cnum_response(hf_peer_addr, number, number_type, service_type);
            break;
        }

        case ESP_HF_VTS_RESPONSE_EVT:
        {
            ESP_LOGI(BT_HF_TAG, "--DTMF code is: %s.", param->vts_rep.code);
            break;
        }

        case ESP_HF_NREC_RESPONSE_EVT:
        {
            ESP_LOGI(BT_HF_TAG, "--NREC status is: %s.", c_nrec_status_str[param->nrec.state]);
            break;
        }

        case ESP_HF_ATA_RESPONSE_EVT:
        {
            ESP_LOGI(BT_HF_TAG, "--Asnwer Incoming Call.");
            char *number = {"123456"};
            esp_hf_ag_answer_call(param->ata_rep.remote_addr,1,0,1,0,number,0);
            break;
        }

        case ESP_HF_CHUP_RESPONSE_EVT:
        {
            ESP_LOGI(BT_HF_TAG, "--Reject Incoming Call.");
            char *number = {"123456"};
            esp_hf_ag_reject_call(param->chup_rep.remote_addr,0,0,0,0,number,0);
            break;
        }

        case ESP_HF_DIAL_EVT:
        {
            if (param->out_call.num_or_loc) {
                if (param->out_call.type == ESP_HF_DIAL_NUM) {
                    // dia_num
                    ESP_LOGI(BT_HF_TAG, "--Dial number \"%s\".", param->out_call.num_or_loc);
                    esp_hf_ag_out_call(param->out_call.remote_addr,1,0,1,0,param->out_call.num_or_loc,0);
                } else if (param->out_call.type == ESP_HF_DIAL_MEM) {
                    // dia_mem
                    ESP_LOGI(BT_HF_TAG, "--Dial memory \"%s\".", param->out_call.num_or_loc);
                    // AG found phone number by memory position
                    bool num_found = true;
                    if (num_found) {
                        char *number = "123456";
                        esp_hf_ag_cmee_send(param->out_call.remote_addr, ESP_HF_AT_RESPONSE_CODE_OK, ESP_HF_CME_AG_FAILURE);
                        esp_hf_ag_out_call(param->out_call.remote_addr,1,0,1,0,number,0);
                    } else {
                        esp_hf_ag_cmee_send(param->out_call.remote_addr, ESP_HF_AT_RESPONSE_CODE_CME, ESP_HF_CME_MEMORY_FAILURE);
                    }
                }
            } else {
                //dia_last
                //refer to dia_mem
                ESP_LOGI(BT_HF_TAG, "--Dial last number.");
            }
            break;
        }
#if (CONFIG_BT_HFP_WBS_ENABLE)
        case ESP_HF_WBS_RESPONSE_EVT:
        {
            ESP_LOGI(BT_HF_TAG, "--Current codec: %s",c_codec_mode_str[param->wbs_rep.codec]);
            break;
        }
#endif
        case ESP_HF_BCS_RESPONSE_EVT:
        {
            ESP_LOGI(BT_HF_TAG, "--Consequence of codec negotiation: %s",c_codec_mode_str[param->bcs_rep.mode]);
            break;
        }
        case ESP_HF_PKT_STAT_NUMS_GET_EVT:
        {
            ESP_LOGI(BT_HF_TAG, "ESP_HF_PKT_STAT_NUMS_GET_EVT: %d.", event);
            break;
        }

        default:
            ESP_LOGI(BT_HF_TAG, "Unsupported HF_AG EVT: %d.", event);
            break;

    }
}

// Функции для управления потоковой передачей аудио
esp_err_t bt_app_audio_streaming_init(const char *server_ip, uint16_t server_port)
{
    audio_stream_config_t config = {
        .server_port = server_port,
        .buffer_size = 4096,
        .sample_rate = 8000,  // Будет обновлено в зависимости от кодека
        .channels = 1,
        .bits_per_sample = 16
    };

    // Копируем IP адрес
    strncpy(config.server_ip, server_ip, sizeof(config.server_ip) - 1);
    config.server_ip[sizeof(config.server_ip) - 1] = '\0';

    esp_err_t ret = audio_streaming_init(&config);
    if (ret == ESP_OK) {
        ESP_LOGI(BT_HF_TAG, "📡 Audio streaming initialized for server %s:%d", server_ip, server_port);
    } else {
        ESP_LOGE(BT_HF_TAG, "❌ Failed to initialize audio streaming: %s", esp_err_to_name(ret));
    }

    return ret;
}

esp_err_t bt_app_audio_streaming_start(void)
{
    esp_err_t ret = audio_streaming_start();
    if (ret == ESP_OK) {
        ESP_LOGI(BT_HF_TAG, "🎵 Audio streaming started");
    } else {
        ESP_LOGE(BT_HF_TAG, "❌ Failed to start audio streaming: %s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t bt_app_audio_streaming_stop(void)
{
    esp_err_t ret = audio_streaming_stop();
    if (ret == ESP_OK) {
        ESP_LOGI(BT_HF_TAG, "⏹️ Audio streaming stopped");
    }
    return ret;
}

esp_err_t bt_app_audio_streaming_deinit(void)
{
    esp_err_t ret = audio_streaming_deinit();
    if (ret == ESP_OK) {
        ESP_LOGI(BT_HF_TAG, "🔌 Audio streaming deinitialized");
    }
    return ret;
}

bool bt_app_audio_streaming_is_connected(void)
{
    return audio_streaming_is_connected();
}
