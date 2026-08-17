/**
 * @file    LivenessWatchdog.h
 * @brief   Signal-only liveness: kick/stall/escalate до reboot.
 *
 * kick() из AudioProducer (samplesTotal). update() на Core 0: stall без kick
 * >WATCHDOG_TIMEOUT_MS → эскалация; MAX_FAILED_RECOVERY + cooldown → reboot.
 * I2S restart — зона SystemMonitor/AudioLifecycle, не здесь.
 *
 * @see SystemMonitor.h, AudioTelemetry.h, docs/ARCHITECTURE.md
 */

#ifndef LIVENESS_WATCHDOG_H
#define LIVENESS_WATCHDOG_H

#include <Arduino.h>
#include <stdint.h>

class LivenessWatchdog {
public:
    static constexpr unsigned long WATCHDOG_TIMEOUT_MS  = 10000;
    static constexpr unsigned long WATCHDOG_CHECK_MS    = 1000;
    static constexpr uint8_t       MAX_FAILED_RECOVERY  = 3;
    static constexpr unsigned long RECOVERY_COOLDOWN_MS = 30000;

    LivenessWatchdog();

    void kick();   ///< Отметка живости (samplesTotal / audio path)
    void update(); ///< Проверка stall и эскалация на Core 0
    void reset();

    bool isAlive()       const { return _alive; }
    bool isStalled()     const { return _stalled; }
    bool isRecovering()  const { return _recovering; }
    uint8_t failCount()  const { return _failCount; }

    void setEnabled(bool en) { _enabled = en; }
    bool isEnabled() const { return _enabled; }

private:
    bool _enabled;
    bool _alive;
    bool _stalled;
    bool _recovering;
    bool _kicked;

    unsigned long _lastKickMs;
    unsigned long _lastCheckMs;
    unsigned long _lastRecoveryMs;
    uint8_t       _failCount;

    void _escalateStall();
};

#endif // LIVENESS_WATCHDOG_H
