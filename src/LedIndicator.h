/**
 * @file    LedIndicator.h
 * @brief   Статусный LED на XIAO ESP32-S3 (GPIO21 / PIN_LED_STATUS).
 *
 * Это НЕ RGB-кольцо микрофона Seeed/XVF3800. Кольцо управляется через
 * XVF3800_I2C::setLedRingEnabled() (I2C LED_EFFECT). Оба связаны в
 * applyLedMode() — см. docs/LED.md.
 *
 * Режимы (неблокирующие; update() из loop):
 *   OFF / STATIC_* / BLINK_* — паттерны по millis
 *   LEVEL — duty 0..255 (legacy; UI больше не выставляет)
 *
 * @see Config.h (PIN_LED_STATUS), docs/LED.md
 */

#ifndef LED_INDICATOR_H
#define LED_INDICATOR_H

#include <Arduino.h>
#include <stdint.h>

#include "Config.h"

class LedIndicator {
public:
    enum Pattern : uint8_t {
        OFF            = 0,  ///< Выключен
        STATIC_ON      = 1,  ///< Постоянно горит
        STATIC_OFF     = 2,  ///< Постоянно выключен
        BLINK_STARTUP  = 3,  ///< 1× 200ms (успешный старт)
        BLINK_ERROR    = 4,  ///< 3× 100ms (ошибка)
        BLINK_NET_OK   = 5,  ///< ровно горит (сеть есть)
        BLINK_NET_FAIL = 6,  ///< 1s blink (нет сети)
        BLINK_WARNING  = 7,  ///< 2× 200ms (предупреждение)
        BLINK_ALARM    = 8,  ///< быстрый flash 100ms (тревога)
        LEVEL          = 9,  ///< уровень 0..255 (duty cycle)
    };

    LedIndicator(uint8_t pin = PIN_LED_STATUS);

    void begin(bool active_high = true);
    void setPattern(Pattern p);
    Pattern getPattern() const;

    /** Установить уровень яркости (0..255), работает только в режиме LEVEL. */
    void setLevel(uint8_t level);

    /** Вызывать из main loop (неблокирующий). */
    void update();

    // Вкл/выкл
    void setEnabled(bool en);
    bool isEnabled() const;

private:
    uint8_t _pin;
    bool    _activeHigh;
    bool    _enabled;
    Pattern _pattern;
    uint8_t _level;       // 0..255 для LEVEL

    // Тайминг паттерна
    unsigned long _lastToggle;
    uint8_t       _blinkPhase;   // 0..n-1
    uint8_t       _blinkCycles;  // сколько раз мигнуть

    /** Выполнить один шаг паттерна. */
    void _executePattern();

    /** Установить физическое состояние LED. */
    void _setPin(uint8_t state);
};

#endif