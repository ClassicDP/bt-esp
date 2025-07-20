#ifndef AUTOSTART_H
#define AUTOSTART_H

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize autostart system
 * @return ESP_OK on success
 */
esp_err_t autostart_init(void);

/**
 * @brief Execute autostart commands
 * @return ESP_OK on success
 */
esp_err_t autostart_execute(void);

/**
 * @brief Save commands to autostart configuration
 * @param commands Array of command strings
 * @param count Number of commands
 * @return ESP_OK on success
 */
esp_err_t autostart_save_commands(const char **commands, size_t count);

/**
 * @brief Load commands from autostart configuration
 * @param commands Output array for command strings (must be freed by caller)
 * @param count Output number of commands
 * @return ESP_OK on success
 */
esp_err_t autostart_load_commands(char ***commands, size_t *count);

/**
 * @brief Clear autostart commands
 * @return ESP_OK on success
 */
esp_err_t autostart_clear(void);

/**
 * @brief Enable/disable autostart
 * @param enable true to enable, false to disable
 * @return ESP_OK on success
 */
esp_err_t autostart_set_enabled(bool enable);

/**
 * @brief Check if autostart is enabled
 * @return true if enabled, false otherwise
 */
bool autostart_is_enabled(void);

#ifdef __cplusplus
}
#endif

#endif // AUTOSTART_H
