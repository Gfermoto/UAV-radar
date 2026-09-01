/**
 * @file    LedIndicator.cpp
 * @brief   Реализация драйвера LED-индикации.
 */

#include "LedIndicator.h"

// PIN_LED_STATUS из Config.h: GPIO_NUM_21 (XIAO ESP32S3 built-in LED)
#ifndef LED_DEFAULT_PIN
#define LED_DEFAULT_PIN  21
#endif

LedIndicator::LedIndicator(uint8_t pin)
    : _pin(pin)
    , _activeHigh(true)
    , _enabled(true)
    , _pattern(LedIndicator::OFF)
    , _level(0)
    , _lastToggle(0)
    , _blinkPhase(0)
    , _blinkCycles(0)
{}

void LedIndicator::begin(bool active_high) {
    _activeHigh = active_high;
    pinMode(_pin, OUTPUT);
    _setPin(LOW);
}

void LedIndicator::setPattern(Pattern p) {
    _pattern = p;
    _blinkPhase = 0;
    _blinkCycles = 0;
    _lastToggle = millis();
    // После analogWrite (LEVEL) GPIO остаётся на LEDC — вернуть OUTPUT.
    pinMode(_pin, OUTPUT);
    if (p == OFF || p == STATIC_OFF) {
        _setPin(LOW);
    } else if (p == STATIC_ON || p == BLINK_NET_OK || p == BLINK_NET_FAIL) {
        _setPin(HIGH);
        if (p == BLINK_NET_FAIL) _blinkPhase = 1;  // первый timeout → LOW
    } else if (p == LEVEL) {
        analogWrite(_pin, _level);
    }
}

LedIndicator::Pattern LedIndicator::getPattern() const {
    return _pattern;
}

void LedIndicator::setLevel(uint8_t level) {
    _level = level;
    if (_pattern == LEVEL && _enabled) {
        analogWrite(_pin, _level);
    }
}

void LedIndicator::update() {
    if (!_enabled) return;
    if (_pattern <= STATIC_OFF || _pattern == LEVEL || _pattern == BLINK_NET_OK) {
        return;  // статические режимы — не требуют обновления
    }
    _executePattern();
}

void LedIndicator::_executePattern() {
    unsigned long now = millis();
    unsigned long elapsed = now - _lastToggle;

    switch (_pattern) {
    case BLINK_STARTUP:
        // 1 × 200ms
        if (_blinkPhase == 0) {
            _setPin(HIGH);
            _lastToggle = now;
            _blinkPhase = 1;
        } else if (_blinkPhase == 1 && elapsed >= LED_BLINK_STARTUP_MS) {
            _setPin(LOW);
            _blinkPhase = 2;  // done
        }
        break;

    case BLINK_ERROR:
        // 3 × 100ms on/off
        if (_blinkCycles < 3 && elapsed >= LED_BLINK_ERROR_ON_MS) {
            _setPin(_blinkPhase % 2 == 0 ? HIGH : LOW);
            _blinkPhase++;
            if (_blinkPhase >= 6) {
                _blinkPhase = 0;
                _blinkCycles = 3;
                _setPin(LOW);
            }
            _lastToggle = now;
        }
        break;

    case BLINK_NET_FAIL:
        // 1s blink: 500ms on, 500ms off
        if (elapsed >= LED_BLINK_NET_FAIL_MS / 2) {
            _setPin(_blinkPhase % 2 == 0 ? HIGH : LOW);
            _blinkPhase++;
            _lastToggle = now;
        }
        break;

    case BLINK_WARNING:
        // 2 × 200ms on/off
        if (_blinkCycles < 2 && elapsed >= LED_BLINK_WARNING_ON_MS) {
            _setPin(_blinkPhase % 2 == 0 ? HIGH : LOW);
            _blinkPhase++;
            if (_blinkPhase >= 4) {
                _blinkPhase = 0;
                _blinkCycles = 2;
                _setPin(LOW);
            }
            _lastToggle = now;
        }
        break;

    case BLINK_ALARM:
        // быстрый flash
        if (elapsed >= LED_BLINK_ALARM_MS) {
            _setPin(_blinkPhase % 2 == 0 ? HIGH : LOW);
            _blinkPhase++;
            _lastToggle = now;
        }
        break;

    default:
        break;
    }
}

void LedIndicator::_setPin(uint8_t state) {
    pinMode(_pin, OUTPUT);
    digitalWrite(_pin, _activeHigh ? state : !state);
}

void LedIndicator::setEnabled(bool en) {
    _enabled = en;
    if (!en) _setPin(LOW);
}

bool LedIndicator::isEnabled() const {
    return _enabled;
}