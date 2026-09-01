/**
 * @file    platform_stubs.cpp
 * @brief   Host stubs: millis, GPIO, Serial/Wire/WiFi для native unit-тестов.
 */

#include "Arduino.h"
#include "Wire.h"
#include "WiFi.h"
#include "test_mocks.h"

#include <unordered_map>

HardwareSerial Serial;
TwoWire Wire;
WiFiClass WiFi;

static uint32_t g_millis = 0;

uint32_t millis() { return g_millis; }

void test_millis_set(uint32_t value) { g_millis = value; }

void test_millis_advance(uint32_t delta) { g_millis += delta; }

#include <unordered_map>

static std::unordered_map<uint8_t, int> g_gpio_state;
static std::unordered_map<uint8_t, int> g_gpio_mode;

void mock_gpio_reset() { g_gpio_state.clear(); g_gpio_mode.clear(); }
int  mock_gpio_get_state(uint8_t pin) {
    auto it = g_gpio_state.find(pin); return it != g_gpio_state.end() ? it->second : -1;
}
int  mock_gpio_get_pinmode(uint8_t pin) {
    auto it = g_gpio_mode.find(pin); return it != g_gpio_mode.end() ? it->second : -1;
}

void pinMode(uint8_t pin, uint8_t mode) { g_gpio_mode[pin] = mode; }
void digitalWrite(uint8_t pin, uint8_t value) { g_gpio_state[pin] = value; }
void analogWrite(uint8_t pin, int value) { g_gpio_state[pin] = value; }

void delay(uint32_t ms) { g_millis += ms; }

int64_t time(nullptr_t) { return static_cast<int64_t>(g_millis / 1000); }
