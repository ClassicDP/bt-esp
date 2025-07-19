#ifndef AUDIO_STREAMING_H
#define AUDIO_STREAMING_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Конфигурация для потоковой передачи аудио
 */
typedef struct {
    char server_ip[16];         /*!< IP адрес сервера */
    uint16_t server_port;       /*!< Порт сервера */
    uint32_t buffer_size;       /*!< Размер буфера для аудиоданных */
    uint32_t sample_rate;       /*!< Частота дискретизации */
    uint8_t channels;           /*!< Количество каналов */
    uint8_t bits_per_sample;    /*!< Бит на сэмпл */
} audio_stream_config_t;

/**
 * @brief Инициализация модуля потоковой передачи аудио
 *
 * @param config Конфигурация потока
 * @return ESP_OK при успехе
 */
esp_err_t audio_streaming_init(const audio_stream_config_t *config);

/**
 * @brief Отправка аудиоданных на сервер
 *
 * @param data Указатель на аудиоданные
 * @param size Размер данных в байтах
 * @return ESP_OK при успехе
 */
esp_err_t audio_streaming_send(const uint8_t *data, uint32_t size);

/**
 * @brief Запуск потоковой передачи
 *
 * @return ESP_OK при успехе
 */
esp_err_t audio_streaming_start(void);

/**
 * @brief Остановка потоковой передачи
 *
 * @return ESP_OK при успехе
 */
esp_err_t audio_streaming_stop(void);

/**
 * @brief Деинициализация модуля
 *
 * @return ESP_OK при успехе
 */
esp_err_t audio_streaming_deinit(void);

/**
 * @brief Проверка состояния соединения
 *
 * @return true если соединение активно
 */
bool audio_streaming_is_connected(void);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_STREAMING_H */
