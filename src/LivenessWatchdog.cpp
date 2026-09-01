/**
 * @file    LivenessWatchdog.cpp
 * @brief   Stall detect + reboot escalate; I2S reset — SystemMonitor.
 */

#include "LivenessWatchdog.h"
#include "esp_system.h"

LivenessWatchdog::LivenessWatchdog()
    : _enabled(true)
    , _alive(true)
    , _stalled(false)
    , _recovering(false)
    , _kicked(false)
    , _lastKickMs(0)
    , _lastCheckMs(0)
    , _lastRecoveryMs(0)
    , _failCount(0)
{}

void LivenessWatchdog::kick() {
    if (!_enabled) return;
    _kicked = true;
    _lastKickMs = millis();
    _alive = true;
    _stalled = false;
    _failCount = 0;
}

void LivenessWatchdog::update() {
    if (!_enabled) return;

    unsigned long now = millis();
    if (now - _lastCheckMs < WATCHDOG_CHECK_MS) return;
    _lastCheckMs = now;
    if (!_kicked) return;

    unsigned long elapsed = now - _lastKickMs;
    if (elapsed > WATCHDOG_TIMEOUT_MS) {
        _alive = false;
        _stalled = true;
        if (now - _lastRecoveryMs > RECOVERY_COOLDOWN_MS) {
            _escalateStall();
        }
    } else {
        _stalled = false;
    }
}

void LivenessWatchdog::_escalateStall() {
    _recovering = true;
    _lastRecoveryMs = millis();
    _failCount++;
    Serial.printf("[LIVENESS] stall escalate count=%u (I2S owner=SystemMonitor)\n",
                  (unsigned)_failCount);

    if (_failCount >= MAX_FAILED_RECOVERY) {
        Serial.printf("[LIVENESS] MAX_FAILED_RECOVERY — reboot\n");
        delay(500);
        esp_restart();
    }

    _recovering = false;
}

void LivenessWatchdog::reset() {
    _alive = true;
    _stalled = false;
    _failCount = 0;
    _recovering = false;
    _kicked = false;
    _lastKickMs = 0;
    _lastCheckMs = 0;
    _lastRecoveryMs = 0;
}
