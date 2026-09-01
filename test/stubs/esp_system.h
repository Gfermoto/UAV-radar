/**
 * @file    esp_system.h
 * @brief   Minimal stub for ESP-IDF esp_system.h — native unit test environment.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void esp_restart(void);

/** @brief esp_reset_reason_t (subset) + stub. */
typedef enum {
    ESP_RST_UNKNOWN = 0,
    ESP_RST_POWERON = 1,
    ESP_RST_BROWNOUT = 15,
    ESP_RST_TASK_WDT = 5,
    ESP_RST_INT_WDT = 6,
    ESP_RST_PANIC = 3,
} esp_reset_reason_t;
esp_reset_reason_t esp_reset_reason(void);

/** @brief Stub temperatureRead — возвращает мок-значение */
float temperatureRead(void);

/** @brief Stub setCpuFrequencyMhz — сохраняет значение в мок */
void setCpuFrequencyMhz(unsigned int freq_mhz);

#ifdef __cplusplus
}
#endif
