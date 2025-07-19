#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include "esp_err.h"
#include "esp_wifi.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief WiFi configuration structure
 */
typedef struct {
    char ssid[32];          /*!< WiFi SSID */
    char password[64];      /*!< WiFi password */
    int max_retry;          /*!< Maximum retry attempts */
} wifi_manager_config_t;

/**
 * @brief Initialize WiFi module
 *
 * @return ESP_OK on success
 */
esp_err_t wifi_manager_init(void);

/**
 * @brief Connect to WiFi network
 *
 * @param ssid WiFi network name
 * @param password WiFi password
 * @return ESP_OK on success
 */
esp_err_t wifi_manager_connect(const char *ssid, const char *password);

/**
 * @brief Disconnect from WiFi
 *
 * @return ESP_OK on success
 */
esp_err_t wifi_manager_disconnect(void);

/**
 * @brief Get WiFi connection status
 *
 * @return true if connected
 */
bool wifi_manager_is_connected(void);

/**
 * @brief Get IP address
 *
 * @param ip_str Buffer to store IP address string
 * @param max_len Maximum buffer length
 * @return ESP_OK on success
 */
esp_err_t wifi_manager_get_ip(char *ip_str, size_t max_len);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_MANAGER_H */
