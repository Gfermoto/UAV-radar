/**
 * @file    SystemMonitor.cpp
 * @brief   Реализация мониторинга здоровья системы.
 *
 * @author  RTSPMIC Team
 * @date    2026-07-01
 */

#include "SystemMonitor.h"
#include "AudioProducer.h"
#include "AudioLifecycle.h"
#include "DiagnosticsNvs.h"
#include <esp_system.h>
#include <WiFi.h>
#include <Preferences.h>

#ifdef UNIT_TEST
#include <ctime>
#endif

SystemMonitor::SystemMonitor(AudioProducer *producer,
                             AudioLifecycleCoordinator *lifecycle)
    : _producer(producer)
    , _lifecycle(lifecycle)
    , _running(false)
    , _throttled(false)
    , _shutdown(false)
    , _lastTemp(0.0f)
    , _lastRecovery(RecoveryLevel::NONE)
    , _lastRecoveryMs(0)
    , _lastCheckMs(0)
    , _audioSilenceStartMs(0)
    , _i2sErrorCount(0)
    , _lastRingBufDrops(0)
    , _ringBufOverflowSeq(0)
    , _scheduledResetEnabled(true)
    , _shutdownStartMs(0)
    , _thermalLatch(false)
    , _audioStoppedForThermal(false)
    , taskHandle(nullptr)
{
    // Восстановление persistent thermal latch из NVS
    Preferences prefs;
    if (prefs.begin("rtspmic", true)) {
        if (prefs.isKey("thermal_latch")) {
            _thermalLatch = prefs.getBool("thermal_latch", false);
            if (_thermalLatch) {
                Serial.printf("[MONITOR] Thermal latch active from previous boot\n");
            }
        }
        prefs.end();
    }
}

void SystemMonitor::begin() {
    if (_running || taskHandle) return;

    _running = true;
    _lastCheckMs = millis();

    // TWDT init is done once in setup() (main) before audio tasks.
    // Scheduled reset flag is owned by NetConfig (key "sched_reset"); in-memory only here.
    Preferences prefs;
    if (prefs.begin("rtspmic", true)) {
        if (prefs.isKey("sched_reset")) {
            _scheduledResetEnabled = prefs.getBool("sched_reset", true);
        } else if (prefs.isKey("sched_reset_enabled")) {
            // Legacy key from older SystemMonitor builds
            _scheduledResetEnabled = prefs.getBool("sched_reset_enabled", true);
        }
        prefs.end();
    }

    xTaskCreatePinnedToCore(
        monitorTask, "sysMonitor", NETWORK_TASK_STACK_SIZE, this,
        NETWORK_CONTROL_PRIORITY - 1, &taskHandle, 0  // Core 0: thermal не блокирует I2S
    );

    Serial.printf("[MONITOR] Started (interval=%d ms, throttle=%.0fC, shutdown=%.0fC, "
                  "sched_reset=%s)\n",
                  CHECK_INTERVAL_MS, TEMP_THROTTLE_C, TEMP_SHUTDOWN_C,
                  _scheduledResetEnabled ? "ON" : "OFF");
}

bool SystemMonitor::isThrottled() const { return _throttled; }
bool SystemMonitor::isShutdown() const  { return _shutdown; }
float SystemMonitor::getTemperature() const { return _lastTemp; }
RecoveryLevel SystemMonitor::getLastRecovery() const { return _lastRecovery; }
uint32_t SystemMonitor::getLastRecoveryTimeMs() const { return _lastRecoveryMs; }

void SystemMonitor::setScheduledResetEnabled(bool enabled) {
    _scheduledResetEnabled = enabled;
    Serial.printf("[MONITOR] Scheduled reset: %s\n", enabled ? "ON" : "OFF");
}

void SystemMonitor::setScheduledResetTime(uint8_t hour, uint8_t minute) {
    if (hour > 23) hour = 3;
    if (minute > 59) minute = 0;
    _schedResetHour = hour;
    _schedResetMinute = minute;
    Serial.printf("[MONITOR] Scheduled reset time: %02u:%02u\n",
                  (unsigned)_schedResetHour, (unsigned)_schedResetMinute);
}

void SystemMonitor::ensureThermalAudioStopped() {
    if (_audioStoppedForThermal) return;
    // AudioLifecycle: pause RTSP/Opus до stop I2S — иначе fanout читает мёртвый ring.
    if (_lifecycle) {
        AudioLifecycleResult result = _lifecycle->pauseAndStop();
        if (result == AudioLifecycleResult::STOPPED) {
            _audioStoppedForThermal = true;
            return;
        }
        Serial.printf("[MONITOR] Thermal stop via lifecycle failed (%d) — fallback\n",
                      (int)result);
    }
    if (_producer) {
        _producer->stop();
        _audioStoppedForThermal = true;
    }
}

void SystemMonitor::checkThermal() {
    float temp = temperatureRead();
    _lastTemp = temp;

    // Persistent thermal latch: не снимать throttle, пока не остынет до TEMP_RECOVER_C
    if (_thermalLatch && temp < TEMP_RECOVER_C) {
        _thermalLatch = false;
        _throttled = false;
        setCpuFrequencyMhz(CPU_FREQ_NORMAL_MHZ);
        Preferences prefs;
        prefs.begin("rtspmic", false);
        prefs.remove("thermal_latch");
        prefs.end();
        Serial.printf("[MONITOR] Thermal recovery: %.1f°C -> CPU=%uMHz\n",
                      temp, CPU_FREQ_NORMAL_MHZ);
    }

    // Уровень 2: SHUTDOWN (>90°C) — безопасная остановка аудио-пайплайна
    if (temp >= TEMP_SHUTDOWN_C && !_shutdown) {
        _shutdown = true;
        _thermalLatch = true;
        _shutdownStartMs = millis();

        Preferences prefs;
        prefs.begin("rtspmic", false);
        prefs.putString("shutdown_reason", "thermal");
        prefs.putFloat("shutdown_temp", temp);
        prefs.putBool("thermal_latch", true);
        prefs.end();

        Serial.printf("[MONITOR] THERMAL SHUTDOWN: %.1f°C! Stopping audio, CPU=80MHz\n", temp);
        setCpuFrequencyMhz(CPU_FREQ_COOLDOWN_MHZ);
        _audioStoppedForThermal = false;
        ensureThermalAudioStopped();
        return;
    }

    // В shutdown: держим audio stopped, ждём остывания, затем restart
    if (_shutdown) {
        ensureThermalAudioStopped();
        if (temp < TEMP_RECOVER_C) {
            if (!_audioStoppedForThermal) {
                Serial.printf("[MONITOR] Thermal cooldown: audio still running — retry stop\n");
                return;
            }
            uint32_t shutdownDuration = millis() - _shutdownStartMs;
            Serial.printf("[MONITOR] Thermal cooldown: %.1f°C after %u sec\n",
                          temp, shutdownDuration / 1000);
            setCpuFrequencyMhz(CPU_FREQ_NORMAL_MHZ);
            bool restarted = false;
            if (_lifecycle) {
                restarted =
                    _lifecycle->startAndResume() == AudioLifecycleResult::STARTED;
            } else if (_producer) {
                restarted = _producer->begin();
            }
            if (restarted) {
                _shutdown = false;
                _audioStoppedForThermal = false;
            } else {
                Serial.printf("[MONITOR] Thermal audio restart failed; consumers paused\n");
            }
        } else {
            Serial.printf("[MONITOR] Thermal cooldown wait: %.1f°C (target < %.0f°C)\n",
                          temp, TEMP_RECOVER_C);
        }
        return;
    }

    // Уровень 1: THROTTLE (>80°C) — снижение CPU до 160 МГц
    if (temp >= TEMP_THROTTLE_C && !_throttled) {
        _throttled = true;
        _thermalLatch = true;
        setCpuFrequencyMhz(CPU_FREQ_THROTTLE_MHZ);
        Serial.printf("[MONITOR] Thermal throttle: %.1f°C -> CPU=%uMHz\n",
                      temp, CPU_FREQ_THROTTLE_MHZ);
    }
    // Возврат из throttle (без latch — обычное остывание)
    else if (temp < TEMP_THROTTLE_C - 5.0f && _throttled && !_thermalLatch) {
        _throttled = false;
        setCpuFrequencyMhz(CPU_FREQ_NORMAL_MHZ);
        Serial.printf("[MONITOR] Thermal normal: %.1f°C -> CPU=%uMHz\n",
                      temp, CPU_FREQ_NORMAL_MHZ);
    }
}

void SystemMonitor::checkAudioWatchdog() {
    // Thermal shutdown owns the audio lifecycle — do not I2S-restart underneath it.
    if (_shutdown) return;
    if (!_producer) return;

    AudioTelemetry telem;
    _producer->getTelemetry(telem);

    uint32_t now = millis();
    uint32_t lastSample = telem.lastSampleTimeMs;
    uint32_t silenceDuration = (lastSample > 0) ? (now - lastSample) : 0;

    if (silenceDuration > AUDIO_SILENCE_TIMEOUT_MS && _audioSilenceStartMs == 0) {
        _audioSilenceStartMs = now;
        Serial.printf("[MONITOR] Audio silence > %d sec!\n",
                      AUDIO_SILENCE_TIMEOUT_MS / 1000);
    }

    uint32_t sinceLastRecovery = now - _lastRecoveryMs;
    if (silenceDuration > AUDIO_SILENCE_TIMEOUT_MS &&
        (now - _audioSilenceStartMs) > AUDIO_SILENCE_TIMEOUT_MS &&
        sinceLastRecovery > RecoveryThresholds::RECOVERY_COOLDOWN_MS) {
        Serial.printf("[MONITOR] Recovery L1: I2S reset\n");
        executeRecovery(RecoveryLevel::I2S_RESET);
        _audioSilenceStartMs = 0;
    }

    if (silenceDuration <= AUDIO_SILENCE_TIMEOUT_MS) {
        _audioSilenceStartMs = 0;
    }

    // Scheduled Reset (ежедневно в HH:MM, только при enabled и NTP synced)
    // День последнего reset в NVS: иначе reboot в окне → повторный reset после NTP.
    if (_scheduledResetEnabled) {
        static uint32_t s_lastResetDay = 0;
        static bool s_dayLoaded = false;
        struct tm timeInfo;
        time_t t = time(nullptr);
        if (t > 1609459200 && localtime_r(&t, &timeInfo)) {
            if (!s_dayLoaded) {
                Preferences p;
                if (p.begin("rtspmic", true)) {
                    s_lastResetDay = p.getUInt("sched_rst_day", 0);
                    p.end();
                }
                s_dayLoaded = true;
            }
            // Локальный календарный день (yday+year), не UTC-сутки
            const uint32_t day = (uint32_t)timeInfo.tm_year * 400u +
                                 (uint32_t)timeInfo.tm_yday;
            // Минуты от полуночи: окно [target, target+5) корректно через 59→00
            const int mins = timeInfo.tm_hour * 60 + timeInfo.tm_min;
            const int target = (int)_schedResetHour * 60 + (int)_schedResetMinute;
            const int diff = mins - target;
            if (diff >= 0 && diff < 5 && s_lastResetDay != day) {
                // Сначала persist в NVS: если запись не удалась — НЕ ребутим,
                // иначе после reboot то же 5-мин окно → цикл перезагрузок.
                bool persisted = false;
                if (DiagnosticsNvs::writeLastEvent("scheduled_reset")) {
                    Preferences prefs;
                    if (prefs.begin(DiagnosticsNvs::kNamespace, false)) {
                        // putUInt==0 при реальной ошибке flash; «same value» здесь
                        // невозможно — выше гейт s_lastResetDay != day
                        persisted = prefs.putUInt("sched_rst_day", day) > 0;
                        prefs.end();
                    }
                }
                if (!persisted) {
                    Serial.printf("[MONITOR] Scheduled reset SKIP: NVS persist failed\n");
                } else {
                    s_lastResetDay = day;
                    Serial.printf("[MONITOR] Scheduled daily reset (%02u:%02u)\n",
                                  (unsigned)_schedResetHour, (unsigned)timeInfo.tm_min);
                    executeRecovery(RecoveryLevel::FULL_REBOOT);
                }
            }
        }
    }
}

void SystemMonitor::checkRingBufferHealth() {
    if (_shutdown) return;
    if (!_producer || !_producer->getRingBuffer()) return;

    AudioTelemetry telem;
    _producer->getTelemetry(telem);

    uint32_t now = millis();
    uint32_t currentDrops = telem.ringBufferDrops;
    if (currentDrops < _lastRingBufDrops) {
        _lastRingBufDrops = currentDrops;
    }
    uint32_t newDrops = currentDrops - _lastRingBufDrops;

    // Утилизация Ring Buffer
    size_t ringAvail = _producer->available();
    float ringUtil = (float)ringAvail / RING_BUFFER_SIZE;

    // Критерий: >90% утилизация + последовательные капли
    if (ringUtil >= RecoveryThresholds::RINGBUF_UTIL_THRESHOLD && newDrops > 0) {
        _ringBufOverflowSeq++;
    } else {
        _ringBufOverflowSeq = 0;
    }

    if (_ringBufOverflowSeq >= RecoveryThresholds::RINGBUF_CONSECUTIVE_OVERFLOW) {
        uint32_t overflowCount = _ringBufOverflowSeq;
        _ringBufOverflowSeq = 0;
        uint32_t sinceLastRecovery = now - _lastRecoveryMs;
        if (sinceLastRecovery > RecoveryThresholds::RECOVERY_COOLDOWN_MS) {
            Serial.printf("[MONITOR] RingBuf overflow (util=%.0f%%, drops=%u/%u), "
                          "recovery L1: I2S reset\n",
                          ringUtil * 100, overflowCount,
                          RecoveryThresholds::RINGBUF_CONSECUTIVE_OVERFLOW);
            executeRecovery(RecoveryLevel::I2S_RESET);
        } else {
            Serial.printf("[MONITOR] RingBuf overflow suppressed (cooldown %d/%d ms)\n",
                          sinceLastRecovery, RecoveryThresholds::RECOVERY_COOLDOWN_MS);
        }
    }

    _lastRingBufDrops = currentDrops;
}

void SystemMonitor::executeRecovery(RecoveryLevel level) {
    _lastRecovery = level;
    _lastRecoveryMs = millis();

    // NVS write rate-limit: при частых I2S_RESET (залипший watchdog) не жечь флеш.
    // FULL_REBOOT и смена уровня — всегда (диагностика важнее).
    static uint32_t s_lastNvsWriteMs = 0;
    static uint8_t  s_lastNvsLevel = 0;
    if (level == RecoveryLevel::FULL_REBOOT ||
        (uint8_t)level != s_lastNvsLevel ||
        (_lastRecoveryMs - s_lastNvsWriteMs) >= 60000) {
        s_lastNvsWriteMs = _lastRecoveryMs;
        s_lastNvsLevel = (uint8_t)level;
        Preferences prefs;
        if (prefs.begin("rtspmic", false)) {
            prefs.putUInt("recovery_level", (uint8_t)level);
            prefs.putUInt("recovery_time", _lastRecoveryMs);
            prefs.end();
        }
    }

    switch (level) {
        case RecoveryLevel::I2S_RESET:
            Serial.printf("[MONITOR] L1: I2S restart\n");
            if (_lifecycle) {
                AudioLifecycleResult result = _lifecycle->restart();
                if (result != AudioLifecycleResult::RESTARTED) {
                    Serial.printf("[MONITOR] L1 restart failed: %d\n", (int)result);
                }
            } else if (_producer) {
                _producer->stop();
                vTaskDelay(pdMS_TO_TICKS(500));
                _producer->begin();
                _lastRingBufDrops = 0;
            }
            break;

        case RecoveryLevel::WIFI_RESET:
            Serial.printf("[MONITOR] L2: WiFi restart\n");
            WiFi.disconnect();
            vTaskDelay(pdMS_TO_TICKS(2000));
            WiFi.reconnect();
            break;

        case RecoveryLevel::FULL_REBOOT:
            Serial.printf("[MONITOR] L3: FULL REBOOT in 3s\n");
            vTaskDelay(pdMS_TO_TICKS(REBOOT_DELAY_MS));
            ESP.restart();
            break;

        default:
            break;
    }
}

void SystemMonitor::monitorTask(void *param) {
    SystemMonitor *self = static_cast<SystemMonitor *>(param);

    while (self->_running) {
        uint32_t now = millis();
        if (now - self->_lastCheckMs >= CHECK_INTERVAL_MS) {
            self->_lastCheckMs = now;
            self->checkThermal();
            self->checkAudioWatchdog();
            self->checkRingBufferHealth();
        }
        TICK_DELAY_MS(1000);
    }

    self->taskHandle = nullptr;
    vTaskDelete(nullptr);
}
