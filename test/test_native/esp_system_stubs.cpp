/**
 * @file    esp_system_stubs.cpp
 * @brief   Stubs ESP-IDF: temperatureRead, setCpuFrequencyMhz, esp_restart для SystemMonitor.
 */

#include <Arduino.h>
#include "test_mocks.h"
#include "esp_system.h"

// Controllable mock state
static float mock_temperature_c = 25.0f;
static uint32_t mock_cpu_freq_mhz = 240;
static int mock_esp_restart_count = 0;

void mock_temperature_set(float celsius) {
    mock_temperature_c = celsius;
}

float mock_temperature_get() {
    return mock_temperature_c;
}

uint32_t mock_cpu_freq_get() {
    return mock_cpu_freq_mhz;
}

void mock_cpu_freq_reset() {
    mock_cpu_freq_mhz = 240;
}

// ---- ESP-IDF stubs ----

extern "C" {

float temperatureRead() {
    return mock_temperature_c;
}

void setCpuFrequencyMhz(uint32_t freq_mhz) {
    mock_cpu_freq_mhz = freq_mhz;
}

void esp_restart() {
    mock_esp_restart_count++;
    // no-op in native tests
}

esp_reset_reason_t esp_reset_reason(void) {
    return ESP_RST_POWERON;
}

} // extern "C"

// ---- Mock helpers (C++ linkage) ----

void mock_esp_restart_reset() { mock_esp_restart_count = 0; }
int  mock_esp_restart_get_count() { return mock_esp_restart_count; }
