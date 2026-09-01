#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <cmath>
#include <string>
#include <cstdarg>

typedef uint8_t byte;

#define HIGH 1
#define LOW 0
#define INPUT 0
#define OUTPUT 1
#define ARDUINO 100

typedef int gpio_num_t;
#define GPIO_NUM_4   4
#define GPIO_NUM_5   5
#define GPIO_NUM_7   7
#define GPIO_NUM_8   8
#define GPIO_NUM_10  10
#define GPIO_NUM_11  11
#define GPIO_NUM_12  12
#define GPIO_NUM_13  13
#define GPIO_NUM_14  14
#define GPIO_NUM_15  15
#define GPIO_NUM_21  21
#define GPIO_NUM_43  43

#include "WString.h"
#include "Print.h"
#include "pgmspace.h"

class HardwareSerial : public Print {
public:
    size_t printf(const char *fmt) {
        return fmt ? std::strlen(fmt) : 0;
    }

    template<typename... Args>
    size_t printf(const char *fmt, Args... args) {
        return static_cast<size_t>(std::snprintf(nullptr, 0, fmt, args...));
    }
};

extern HardwareSerial Serial;

uint32_t millis();
void test_millis_set(uint32_t value);
void test_millis_advance(uint32_t delta);

void pinMode(uint8_t pin, uint8_t mode);
void digitalWrite(uint8_t pin, uint8_t value);
void analogWrite(uint8_t pin, int value);
void delay(uint32_t ms);

extern "C" uint32_t esp_random();
