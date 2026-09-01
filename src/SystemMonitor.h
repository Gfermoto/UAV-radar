/**
 * @file    SystemMonitor.h
 * @brief   Мониторинг здоровья системы: thermal, audio watchdog, auto-recovery. Core 0.
 *
 * ## Thermal
 * >80°C — throttle CPU (160 MHz); >90°C — shutdown аудио + latch до <70°C.
 *
 * ## Recovery (RecoveryLevel)
 * I2S_RESET → WIFI_RESET → FULL_REBOOT при ring overflow / audio silence / I2S errors.
 * RECOVERY_COOLDOWN_MS — защита от recovery loop.
 *
 * ## Watchdog
 * Audio silence >5 с, ring buffer util >90%, scheduled reset (опц. 3:00).
 *
 * @see AudioLifecycle.h, LivenessWatchdog.h, docs/ARCHITECTURE.md
 *
 * @author  RTSPMIC Team
 * @date    2026-07-01
 */

#ifndef SYSTEM_MONITOR_H
#define SYSTEM_MONITOR_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <atomic>
#include "Config.h"

class AudioProducer;
class AudioLifecycleCoordinator;

/** Уровни восстановления */
enum class RecoveryLevel : uint8_t {
    NONE      = 0,
    I2S_RESET = 1,
    WIFI_RESET = 2,
    FULL_REBOOT = 3
};

/** Пороги auto-recovery */
namespace RecoveryThresholds {
    /** Допустимая утилизация Ring Buffer (доля) — при превышении срабатывает recovery */
    constexpr float RINGBUF_UTIL_THRESHOLD = 0.9f;
    /** Минимальный интервал между recovery-операциями (мс) — защита от recovery loop */
    constexpr uint32_t RECOVERY_COOLDOWN_MS = 60000;
    /** Количество последовательных ring buffer оверранов до срабатывания */
    constexpr uint32_t RINGBUF_CONSECUTIVE_OVERFLOW = 5;
}

class SystemMonitor {
public:
    explicit SystemMonitor(AudioProducer *producer,
                           AudioLifecycleCoordinator *lifecycle = nullptr);
    void begin();
    bool isThrottled() const;
    bool isShutdown() const;
    float getTemperature() const;
    RecoveryLevel getLastRecovery() const;
    uint32_t getLastRecoveryTimeMs() const;

    /** @brief Включение/отключение ежедневного scheduled reset (3:00) */
    void setScheduledResetEnabled(bool enabled);
    /** @brief Время ежедневного reset (час 0..23, минута 0..59). */
    void setScheduledResetTime(uint8_t hour, uint8_t minute);

    TaskHandle_t taskHandle;

private:
    AudioProducer  *_producer;
    AudioLifecycleCoordinator *_lifecycle;
    std::atomic<bool> _running{false};
    std::atomic<bool> _throttled{false};
    std::atomic<bool> _shutdown{false};
    float             _lastTemp;
    RecoveryLevel     _lastRecovery;
    uint32_t          _lastRecoveryMs;
    uint32_t          _lastCheckMs;
    uint32_t          _audioSilenceStartMs;
    uint32_t          _i2sErrorCount;
    uint32_t          _lastRingBufDrops;
    uint32_t          _ringBufOverflowSeq;     ///< Последовательные переполнения
    bool              _scheduledResetEnabled;  ///< Ежедневный reset вкл/выкл
    uint8_t           _schedResetHour    = 3;   ///< Час ежедневного reset
    uint8_t           _schedResetMinute  = 0;   ///< Минута ежедневного reset
    uint32_t          _shutdownStartMs;        ///< Время начала shutdown (>90°C)
    bool              _thermalLatch;           ///< Persistent latch — не сбрасывать до остывания
    bool              _audioStoppedForThermal; ///< true after confirmed pauseAndStop/stop

    /** Retry until audio pipeline is stopped (thermal path). */
    void ensureThermalAudioStopped();

    /** Thermal пороги */
    static constexpr float TEMP_THROTTLE_C  = 80.0f;  ///< Снижение CPU
    static constexpr float TEMP_SHUTDOWN_C  = 90.0f;  ///< Аварийное отключение аудио
    static constexpr float TEMP_RECOVER_C   = 70.0f;  ///< Восстановление после shutdown
    static constexpr uint32_t CHECK_INTERVAL_MS = 5000;  ///< Интервал проверки

    /** Thermal частоты CPU */
    static constexpr uint32_t CPU_FREQ_THROTTLE_MHZ = 160;  ///< CPU при throttle (>80°C)
    static constexpr uint32_t CPU_FREQ_COOLDOWN_MHZ = 80;   ///< CPU при shutdown (>90°C)
    static constexpr uint32_t CPU_FREQ_NORMAL_MHZ    = 240; ///< Нормальный режим

    /** Watchdog пороги */
    static constexpr uint32_t AUDIO_SILENCE_TIMEOUT_MS = 5000;   ///< 5 сек без аудио

    void checkThermal();
    void checkAudioWatchdog();
    void checkRingBufferHealth();
    void executeRecovery(RecoveryLevel level);
    static void monitorTask(void *param);

#ifdef UNIT_TEST
public:
    /** @cond TEST — доступ к приватным методам для unit-тестов */
    void test_checkThermal()            { checkThermal(); }
    void test_checkRingBufferHealth()   { checkRingBufferHealth(); }
    void test_setShutdown(bool v)       { _shutdown = v; }
    void test_setThrottled(bool v)      { _throttled = v; }
    void test_setThermalLatch(bool v)   { _thermalLatch = v; }
    void test_setLastRecoveryMs(uint32_t t) { _lastRecoveryMs = t; }
    /** @endcond */
#endif
};

#endif // SYSTEM_MONITOR_H
