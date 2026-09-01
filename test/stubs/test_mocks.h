/**
 * @file    test_mocks.h
 * @brief   Mock-функции для управления состоянием стабов в native unit-тестах.
 */

#ifndef TEST_MOCKS_H
#define TEST_MOCKS_H

#include <Arduino.h>
#include "AudioTelemetry.h"

// ---- GPIO mocks ----
void     mock_gpio_reset();
int      mock_gpio_get_state(uint8_t pin);
int      mock_gpio_get_pinmode(uint8_t pin);

// ---- ESP system mocks ----
void     mock_temperature_set(float celsius);
float    mock_temperature_get();
uint32_t mock_cpu_freq_get();
void     mock_cpu_freq_reset();

// ---- AudioProducer mocks ----
void     mock_audio_set_telemetry(const AudioTelemetry &t);
void     mock_audio_set_available(size_t avail);
void     mock_audio_set_ringbuf(void *ptr);
void     mock_audio_set_begin_result(bool ok);
int      mock_audio_get_stop_calls();
int      mock_audio_get_begin_calls();
void     mock_audio_reset();

// ---- LivenessWatchdog mocks ----
void     mock_esp_restart_reset();
int      mock_esp_restart_get_count();

#endif // TEST_MOCKS_H
