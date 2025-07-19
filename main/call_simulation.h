#ifndef __CALL_SIMULATION_H__
#define __CALL_SIMULATION_H__

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Начать симуляцию входящего звонка для активации микрофона
 *
 * @param auto_answer true - автоматически "ответить" на звонок через 2 секунды
 * @return esp_err_t ESP_OK в случае успеха
 */
esp_err_t start_incoming_call_simulation(bool auto_answer);

/**
 * @brief Ответить на симулированный входящий звонок
 *
 * @return esp_err_t ESP_OK в случае успеха
 */
esp_err_t answer_simulated_call(void);

/**
 * @brief Завершить симулированный звонок
 *
 * @return esp_err_t ESP_OK в случае успеха
 */
esp_err_t end_simulated_call(void);

/**
 * @brief Начать мониторинг уровня микрофона
 *
 * @return esp_err_t ESP_OK в случае успеха
 */
esp_err_t start_microphone_level_monitoring(void);

/**
 * @brief Остановить мониторинг уровня микрофона
 *
 * @return esp_err_t ESP_OK в случае успеха
 */
esp_err_t stop_microphone_level_monitoring(void);

/**
 * @brief Проверка статуса мониторинга микрофона
 *
 * @return bool true если мониторинг активен
 */
bool is_microphone_monitoring_active(void);

#ifdef __cplusplus
}
#endif

#endif /* __CALL_SIMULATION_H__ */
