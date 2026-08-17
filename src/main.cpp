/**
 * @file    main.cpp
 * @brief   Точка входа прошивки RTSP-микрофона для записи природы.
 *
 * ## Обзор архитектуры
 *
 * Двухъядерная архитектура ESP32-S3 (240 МГц, 8 МБ PSRAM). Жёсткое разделение:
 * Core 1 — только аудио (реального времени), Core 0 — сеть и управление.
 *
 * ```
 * ┌──────────────────────────────────────────────────────────────────┐
 * │  Core 1 (AudioProducerTask, приоритет 10) — АУДИОПОТОК          │
 * │                                                                  │
 * │  I2S DMA (аппаратный, 0% CPU)                                   │
 * │    → convertI2SToMono (32→16 бит, стерео→моно, gain)            │
 * │    → xRingbufferSend (lock-free, ~0.5 мкс на сэмпл)              │
 * │    → AudioTelemetry (RMS/peak/clipping/count)                    │
 * │    → MEL-спектрограмма (Radix-2 FFT 512-pt, 64 band, 10ms hop)  │
 * │    → аппаратный HPF XVF (125 Гц) через I2C, не soft-filter      │
 * │                                                                  │
 * │  Загрузка: ~5-35% CPU. ~965-995 мс/с в idle.                    │
 * │  КРИТИЧЕСКИ: Core 1 НИКОГДА не выполняет сетевых операций.       │
 * └──────────────────────────────────────────────────────────────────┘
 * ┌──────────────────────────────────────────────────────────────────┐
 * │  Core 0 (loop + задачи, приоритет 5) — СЕТЬ + УПРАВЛЕНИЕ         │
 * │                                                                  │
 * │  WiFi/captive portal (WiFiSetup), mDNS                          │
 * │  NTP; sensorPoll: XVF3800_Cache + MQTT telemetry 1 с            │
 * │  SystemMonitor task (thermal, scheduled reset)                   │
 * │  Opus encode (фоновая задача Core 0)                             │
 * │  RTSP :554 + optional remote client                              │
 * │  WebUI + WebSocket + OTA                                         │
 * │                                                                  │
 * │  Загрузка: ~35-45% CPU. ~55-65% свободно.                        │
 * │  Opus читает PCM из xRingbuffer (lock-free, cross-core).         │
 * └──────────────────────────────────────────────────────────────────┘
 * ```
 *
 * ## Порядок инициализации (setup)
 *
 *   1. Serial + NVS boot diagnostics (last_event)
 *   2. TWDT 30s, device identity, Wi-Fi / captive portal
 *   3. mDNS (rtsp-mic.local)
 *   4. XVF3800 I2C (AGC, echo suppression, beamforming)
 *   5. NTP синхронизация
 *   6. TelemetryBuilder + LedIndicator
 *   7. AudioProducer (Core 1) + OpusEncoder (Core 0)
 *   8. RTSP Server + Client
 *   9. MQTT + CommandDispatcher
 *  10. WebUI
 *  11. Ethernet (optional W5500)
 *  12. SystemMonitor task + liveness
 *
 * ## Основной цикл (loop, Core 0)
 *
 * Выполняется каждые 10 мс:
 *   1. processDNS() — captive portal
 *   2. updateWiFiRecovery() — reconnect / wifi-dead reboot
 *   3. LivenessWatchdog — kick по samplesTotal
 *   4. NTPClient::update() — ресинхронизация раз в час
 *   5. LedIndicator — status / level patterns
 *   6. MQTTManager::update() — команды (телеметрия в sensorPoll)
 *   7. Ethernet update (если включён)
 *
 * ## Добавление нового модуля
 *
 *   1. Создать заголовок (.h) и реализацию (.cpp) в src/
 *   2. Объявить глобальный экземпляр в секции глобальных объектов
 *   3. Добавить инициализацию в setup() (в правильном порядке!)
 *   4. Добавить update() в loop() если модуль требует периодического вызова
 *   5. При необходимости conditional compilation: #if FIRMWARE_*
 *   6. Для задач реального времени на Core 1: использовать AudioProducer как образец
 *
 * ## Отладка
 *
 * Логи через Serial (115200):
 *   - [CORE0] — Core 0 (сеть, управление)
 *   - [CORE1] — Core 1 (аудио)
 *   - [OPUS]  — OpusEncoder
 *   - [WIFI]  — WiFiSetup
 *   - [WEB]   — WebUI
 *   - [MEL]   — MelSpectrogram
 *   - [CMD]   — MQTT команды
 *
 * Статистика каждые 30 секунд: свободная память, стеки задач, Ring Buffer,
 * активные RTSP-клиенты.
 *
 * @author  RTSP Mic Team
 * @date    2026-07-01
 */

#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "NetConfig.h"
#include "Config.h"

#include "XVF3800_I2C.h"
#include "AudioProducer.h"
#include "NTPClient.h"
#include "RTSPServer.h"
#include "RTSPClient.h"
#include "MQTTManager.h"
#include "WebUI.h"
#include "SystemMonitor.h"
#include "LivenessWatchdog.h"
#include "TelemetryBuilder.h"
#include "WiFiSetup.h"
#include "LedIndicator.h"
#include "XVF3800_Cache.h"
#include "OpusEncoder.h"
#include "AudioLifecycle.h"
#include "EncodedAudioFanout.h"
#include "WiFiRecovery.h"
#include "CommandDispatcher.h"
#include "DiagnosticsNvs.h"
#include <esp_task_wdt.h>
#include <nvs_flash.h>

#if ETHERNET_ENABLED
#include "EthernetManager.h"
#endif

static constexpr uint16_t kRtspLocalPort = 554;

// =============================================================================
//  Глобальные объекты (singleton-like, живут весь цикл работы устройства)
//  Порядок объявления важен для деструкторов (обратный порядок).
// =============================================================================

/** Управление аппаратным DSP XVF3800 через I2C. */
static XVF3800_I2C      _xvf;

/** Кэш телеметрии XVF3800 (DoA, VAD, AGC) — читается раз в 100 мс. */
static XVF3800_Cache     _xvfCache(&_xvf);

/** Производитель аудио: I2S DMA, HPF, MEL, Ring Buffer. Core 1. */
static AudioProducer     _audioProducer;
static EncodedAudioFanout _audioFanout(ENCODED_AUDIO_QUEUE_DEPTH);

/** LED-индикатор (GPIO21): паттерны статуса, яркость по RMS. */
static LedIndicator       _ledIndicator;

/** NTP-клиент: синхронизация времени при старте, ресинхронизация раз в час. */
static NTPClient         _ntp;

/** Локальный RTSP-сервер: раздача аудио клиентам в локальной сети. */
static RTSPServer        _rtspServer;

/** RTSP-клиент: отправка аудио на внешний рекордер или NVR. */
static RTSPClient        _rtspClient;

/** MQTT-клиент: телеметрия + команды управления. */
static MQTTManager       _mqtt;

/** Веб-интерфейс: Dashboard, Network, Audio, System, OTA. */
static WebUI             _webUi;

/** Opus-кодировщик: сжатие аудио на Core 0 (включается через OPUS_ENABLED). */
#if OPUS_ENABLED
static OpusEncoderTask   _opusEncoder;
#endif

#if OPUS_ENABLED
static AudioLifecycleConsumer *_audioConsumers[] = {
    &_opusEncoder, &_rtspServer, &_rtspClient
};
#else
static AudioLifecycleConsumer *_audioConsumers[] = {
    &_rtspServer, &_rtspClient
};
#endif
static AudioLifecycleCoordinator _audioLifecycle(
    _audioProducer, _audioConsumers,
    sizeof(_audioConsumers) / sizeof(_audioConsumers[0]));

/** Системный монитор: watchdog, температура, свободная память. */
static SystemMonitor _sysMonitor(&_audioProducer, &_audioLifecycle);

/** Audio liveness: 10с без фрейма → I2S reset; 3 stall → reboot. */
static LivenessWatchdog _liveness;

#if ETHERNET_ENABLED
static EthernetManager   _ethManager;
#endif

/// @name Параметры подключения (defaults; runtime from NVS via NetConfig)
/// @{
static NetConfigData _netCfg;
static WiFiRecovery  _wifiRecovery(WIFI_CHECK_INTERVAL_MS, WIFI_RECONNECT_TIMEOUT_MS);

static void syncDspPathGainsTelemetry() {
    TelemetryBuilder::setDspPathGains(
        _netCfg.dspMicGain, _netCfg.asroutGain, _netCfg.asroutEnabled,
        _netCfg.attnsMode, _netCfg.attnsNominal, _netCfg.attnsSlope);
}
/// @}

// =============================================================================
//  Внутренние функции (setup)
// =============================================================================

/**
 * @brief Инициализация mDNS для обнаружения устройства в локальной сети.
 *
 * Регистрирует сервисы:
 *   - rtsp._tcp на порту 554
 *   - http._tcp на порту WEB_PORT
 *
 * Имя хоста задаёт WiFiSetup::hostname().
 */
static bool s_mdnsStarted = false;

static void setupMDNS() {
    const char *host = WiFiSetup::hostname();
    if (s_mdnsStarted) {
        MDNS.end();
        s_mdnsStarted = false;
    }
    if (!MDNS.begin(host)) {
        Serial.printf("[CORE0] mDNS init failed\n");
        return;
    }
    MDNS.addService("rtsp", "tcp", kRtspLocalPort);
    MDNS.addService("http", "tcp", WEB_PORT);
    s_mdnsStarted = true;
    Serial.printf("[CORE0] mDNS: %s.local (RTSP:%d, HTTP:%d)\n",
                  host, kRtspLocalPort, WEB_PORT);
}

/**
 * Применить режим «Кольцо» (NVS LedMode) к обоим индикаторам.
 *
 * 1) LedIndicator (GPIO21) — статус Wi‑Fi / off / legacy LEVEL.
 * 2) XVF RGB ring — LED_EFFECT 0 (off) или 4 (DoA). Не breath/rainbow/color.
 *
 * @see docs/LED.md, XVF3800_I2C::setLedRingEnabled
 */
static uint8_t s_ledModeApplied = LED_MODE_STATUS;

static void applyLedMode(uint8_t mode) {
    s_ledModeApplied = mode;
    const bool ringOn = (mode != LED_MODE_OFF);
    switch (mode) {
        case LED_MODE_OFF:
            _ledIndicator.setPattern(LedIndicator::OFF);
            break;
        case LED_MODE_LEVEL:
            _ledIndicator.setPattern(LedIndicator::LEVEL);
            break;
        case LED_MODE_STATUS:
        default: {
            const bool up = (WiFi.status() == WL_CONNECTED);
            _ledIndicator.setPattern(up ? LedIndicator::BLINK_NET_OK
                                        : LedIndicator::BLINK_NET_FAIL);
            break;
        }
    }
    {
        XVF3800_Result rr = _xvf.setLedRingEnabled(ringOn);
        Serial.printf("[LED] XVF ring %s (effect %u) → %s\n",
                      ringOn ? "doa" : "off",
                      (unsigned)(ringOn ? XVF_LED_EFFECT_DOA : XVF_LED_EFFECT_OFF),
                      XVF3800_I2C::resultToString(rr));
    }
    TelemetryBuilder::setSecurityControls(mode);
}

/** @brief Мониторинг Wi-Fi + reconnect с backoff. Полный STA reset, не "залипший" reconnect. */
static void updateWiFiRecovery() {
    static bool wasConnected = false;
    static bool primed = false;
    const bool connected = (WiFi.status() == WL_CONNECTED);
    // Первый тик после setup(): STA уже поднят и mDNS уже вызван — не считать
    // это "restored" (иначе повторный MDNS.begin → Failed adding service).
    if (!primed) {
        wasConnected = connected;
        primed = true;
    }
    _wifiRecovery.tick(millis(), connected);
    if (_wifiRecovery.consumeConnectRequest()) {
        Serial.printf("[CORE0] Wi-Fi full STA reset + reconnect (backoff)...\n");
        WiFiSetup::fullStaResetAndReconnect();
    }
    // Re-publish mDNS after WiFi reconnect (mDNS lost on disconnect)
    if (connected && !wasConnected) {
        Serial.printf("[CORE0] Wi-Fi restored IP=%s RSSI=%d\n",
                      WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
        setupMDNS();
        _mqtt.publishStatus("online");
    } else if (!connected && wasConnected) {
        Serial.printf("[CORE0] Wi-Fi lost status=%d\n", (int)WiFi.status());
    }

    // Wi-Fi watchdog: dead > WIFI_DEAD_REBOOT_MS → reboot (иначе плата живёт без сети).
    // Не ребутим, если сеть есть через Ethernet, активен portal или нет credentials —
    // иначе reboot-loop на живом Eth / в режиме настройки.
    static uint32_t s_wifiDeadSinceMs = 0;
    if (!connected) {
#if ETHERNET_ENABLED
        const bool netAlive = _ethManager.isConnected();
#else
        const bool netAlive = false;
#endif
        if (netAlive || WiFiSetup::isConfigMode() || !WiFiSetup::hasCredentials()) {
            s_wifiDeadSinceMs = 0;
        } else {
            if (s_wifiDeadSinceMs == 0) s_wifiDeadSinceMs = millis();
            if (millis() - s_wifiDeadSinceMs >= WIFI_DEAD_REBOOT_MS) {
                Serial.printf("[WIFI] dead > %u ms → reboot\n", (unsigned)WIFI_DEAD_REBOOT_MS);
                DiagnosticsNvs::writeLastEvent("wifi_dead_reboot");
                vTaskDelay(pdMS_TO_TICKS(500));
                ESP.restart();
            }
        }
    } else {
        s_wifiDeadSinceMs = 0;
    }

    wasConnected = connected;
}

/** XVF telemetry remains independent from Arduino loop. */
static void sensorPollTask(void * /*param*/) {
    uint32_t lastTelemetryMs = 0;
    for (;;) {
        _xvfCache.update();
        TelemetryBuilder::noteSensorTick();
        TelemetryBuilder::setLocalRtspClientCount(_rtspServer.getActiveClientCount());

        const uint32_t now = millis();
        if ((uint32_t)(now - lastTelemetryMs) >= 1000u) {
            _mqtt.publishTelemetry();
            lastTelemetryMs = now;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void startSensorPollTask() {
    static TaskHandle_t s_handle = nullptr;
    if (s_handle) return;
    // Core 0: I2C XVF + MQTT telemetry не в loop() — стабильный 1 Гц независимо от WebUI.
    BaseType_t ok = xTaskCreatePinnedToCore(
        sensorPollTask, "sensorPoll",
        8192, nullptr,
        NETWORK_CONTROL_PRIORITY, &s_handle, 0);
    if (ok != pdPASS) {
        s_handle = nullptr;
        Serial.printf("[CORE0] sensorPoll task create failed\n");
    } else {
        Serial.printf("[CORE0] sensorPoll task started\n");
    }
}

/**
 * @brief Вывод сводной информации о системе в Serial при старте.
 *
 * Включает: имя прошивки, версию, память, IP и локальные порты.
 */
static void printSystemInfo() {
    Serial.printf("\n============================================\n");
    Serial.printf("  %s v%s\n", FIRMWARE_NAME, FIRMWARE_VERSION);
    Serial.printf("  Platform: ESP32S3 @ 240 MHz\n");
    Serial.printf("  RAM: %d KB free\n", ESP.getFreeHeap() / 1024);
    Serial.printf("  PSRAM: %d KB free\n", ESP.getFreePsram() / 1024);
    Serial.printf("  Wi-Fi: %s\n", WiFi.localIP().toString().c_str());
    Serial.printf("  RTSP local: port %d\n", kRtspLocalPort);
    Serial.printf("  Web UI: port %d\n", WEB_PORT);
    Serial.printf("  MQTT local: port %d\n", MQTT_LOCAL_PORT);
    Serial.printf("============================================\n\n");
}

// =============================================================================
//  SETUP — инициализация всех подсистем
// =============================================================================

void setup() {
    // ── 1. Serial ──
    Serial.begin(SERIAL_BAUD_RATE);
    vTaskDelay(pdMS_TO_TICKS(1000));  // Пауза для стабилизации USB-UART

    // ── NVS init (гарантированно, до любого Preferences) ──
    {
        esp_err_t err = nvs_flash_init();
        if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            // Полный erase NVS — иначе Preferences/Wi-Fi «пропадут» без Factory reset в логе.
            Serial.printf("[CORE0] WARN: nvs_flash_erase() reason=%s\n", esp_err_to_name(err));
            ESP_ERROR_CHECK(nvs_flash_erase());
            err = nvs_flash_init();
        }
        if (err != ESP_OK) {
            Serial.printf("[CORE0] NVS flash init failed: %s\n", esp_err_to_name(err));
        }
    }

    // ── Reset reason + NVS last_event (диагностика отвалов) ──
    const esp_reset_reason_t rr = esp_reset_reason();
    {
        const DiagnosticsNvs::BootInfo boot = DiagnosticsNvs::readBoot();
        Serial.printf("[BOOT] Reset reason: %d; last_event=%s last_error=%s\n",
                      (int)rr, boot.lastEvent.c_str(), boot.lastError.c_str());
        const char *ev = boot.lastEvent.length() ? boot.lastEvent.c_str()
                        : (boot.lastError.length() ? boot.lastError.c_str() : "");
        TelemetryBuilder::setResetInfo((int)rr, ev);
        if (boot.lastEvent.length() || boot.lastError.length()) {
            DiagnosticsNvs::clearBootKeys();
        }
    }

    Serial.printf("\n\n[CORE0] ====== Initializing (%s) ======\n", FIRMWARE_NAME);
    Serial.printf("[CORE0] FreeRTOS tasks: %d\n", uxTaskGetNumberOfTasks());

    WiFiSetup::initDeviceIdentity();
    Serial.printf("[CORE0] Device: %s  mDNS: %s.local  (BOOT hold %us = factory reset; off while USB CDC open)\n",
                  WiFiSetup::deviceId(), WiFiSetup::hostname(),
                  (unsigned)(FACTORY_RESET_HOLD_MS / 1000u));
    pinMode(PIN_BTN_FACTORY_RESET, INPUT_PULLUP);

    // ── 2. Wi-Fi + Captive Portal ──
    if (!WiFiSetup::ensureConnection()) {
        Serial.printf("[CORE0] Wi-Fi not connected. Offline mode.\n");
    }

    // ── 3. mDNS ──
    if (WiFi.status() == WL_CONNECTED) {
        setupMDNS();
    }

    // ── 4. XVF3800 DSP (I2C) ──
    Serial.printf("[CORE0] Initializing XVF3800 I2C...\n");
    if (_xvf.begin()) {
        _xvf.setEchoSuppression(true);
        _xvf.setFixedBeam(XVF3800_BeamMode::ADAPTIVE);
        // begin()/version уже доказали slave — не держим xvf_dead
        // пока первый poll ловит 0x40 после applyEngineerConfig.
        _xvfCache.markXvfSeen();
    } else {
        Serial.printf("[CORE0] XVF3800 not found. Audio/DOA unavailable.\n");
    }

    // ── 5. NetConfig (NVS) + NTP ──
    NetConfig::load(_netCfg);
    Serial.printf("[CORE0] NTP sync (%s)...\n", _netCfg.ntpHost);
    _ntp.setServer(_netCfg.ntpHost);
    if (!_ntp.begin()) {
        Serial.printf("[CORE0] NTP not synced.\n");
    }

    // ── 6. TelemetryBuilder (нужен nodeId) ──
    char nodeId[NODE_ID_LEN];
    uint8_t mac[6];
    WiFi.macAddress(mac);
    // Уникальный ID из трёх младших байт MAC-адреса
    snprintf(nodeId, sizeof(nodeId), NODE_ID_FMT, mac[3], mac[4], mac[5]);
    TelemetryBuilder::init(&_audioProducer, &_xvfCache, &_ntp,
                           &_rtspClient, nodeId);

    // ── 7. LED Indicator ──
    _ledIndicator.begin(true);  // active_high
    _ledIndicator.setPattern(LedIndicator::BLINK_STARTUP);

    // ── 8. AudioProducer (Core 1) — КРИТИЧЕСКИЙ ──
    Serial.printf("[CORE0] Starting AudioProducer (Core 1)...\n");
    if (!_audioProducer.begin()) {
        // Без аудио устройство бесполезно — мигаем LED и висим
        Serial.printf("[CORE0] CRITICAL: AudioProducer failed.\n");
        _ledIndicator.setPattern(LedIndicator::BLINK_ERROR);
        while (1) {
            _ledIndicator.update();
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    // ── 9. Apply persisted audio / system settings (after AudioProducer mutex exists) ──
    TelemetryBuilder::setCalibrationOffsetDb(_netCfg.calibrationOffsetDb);
    _sysMonitor.setScheduledResetEnabled(_netCfg.scheduledReset);
    _sysMonitor.setScheduledResetTime(_netCfg.scheduledResetHour, _netCfg.scheduledResetMinute);
    TelemetryBuilder::setDspAgcState(_netCfg.dspAgcEnabled);
    TelemetryBuilder::setDspLimiterState(_netCfg.dspLimiterEnabled);
    TelemetryBuilder::setAecEnvState(_netCfg.aecEnvMode);
    TelemetryBuilder::setEchoSuppressionState(_netCfg.echoSuppressionEnabled);
    TelemetryBuilder::setAsroutState(_netCfg.asroutEnabled);
    TelemetryBuilder::setLoudspeakerPresentState(_netCfg.loudspeakerPresent);
    TelemetryBuilder::setDspMicGainState(_netCfg.dspMicGain);
    syncDspPathGainsTelemetry();
    TelemetryBuilder::setSecurityControls(_netCfg.security.ledMode);
    TelemetryBuilder::setAudioSetupMode(_netCfg.audioSetupMode);
    _xvf.applyEngineerConfig(_netCfg);
    // После пачки write — дать XVF отдышаться и переоткрыть шину.
    delay(50);
    (void)_xvf.recoverBus();
    // Restore ring: off → LED_EFFECT 0; on → DoA (Seeed default), not rainbow.
    applyLedMode(_netCfg.security.ledMode);
    // Опрос DoA/beams/VAD независимо от Arduino loop (часто не крутится с AsyncWebServer).
    startSensorPollTask();
    _audioProducer.setXvfPointer(&_xvf);
    _audioProducer.setHpfMode(_netCfg.hpfMode);

    // ── 10. OpusEncoder (Core 0) ──
    Serial.printf("[CORE0] Starting OpusEncoder (Core 0)...\n");
#if OPUS_ENABLED
    if (!_opusEncoder.begin(&_audioProducer, &_audioFanout)) {
        Serial.printf("[CORE0] OpusEncoder failed (continuing without Opus).\n");
    }
#else
    Serial.printf("[CORE0] Opus disabled (OPUS_ENABLED=0).\n");
#endif

    // ── 10. RTSP Server + Client ──
    _rtspServer.setDependencies(&_audioFanout);
    if (!_rtspServer.begin()) {
        Serial.printf("[CORE0] Local RTSP server failed.\n");
    }


    _rtspClient.setDependencies(&_audioFanout);
    _rtspClient.setRuntimeEnabled(_netCfg.rtspRemoteEnabled);
    if (_netCfg.rtspRemoteEnabled && _netCfg.rtspRemoteHost[0]) {
        _rtspClient.setServer(_netCfg.rtspRemoteHost, _netCfg.rtspRemotePort);
        if (!_rtspClient.begin()) {
            Serial.printf("[CORE0] Remote RTSP client failed.\n");
        }
    } else {
        Serial.printf("[CORE0] Remote RTSP client disabled.\n");
    }

    // ── 11. MQTT ──
    _mqtt.setDependencies(&_ntp);

    static CommandDispatcher::Deps s_cmdDeps;
    s_cmdDeps.netCfg = &_netCfg;
    s_cmdDeps.xvf = &_xvf;
    s_cmdDeps.audio = &_audioProducer;
    s_cmdDeps.sysMonitor = &_sysMonitor;
    s_cmdDeps.applyLedMode = applyLedMode;
    s_cmdDeps.syncDspPathGains = syncDspPathGainsTelemetry;
    static CommandDispatcher s_commands(s_cmdDeps);
    auto onCommand = [](const char *cmd, const JsonVariant &value) {
        s_commands.dispatch(cmd, value);
    };
    _mqtt.setCommandCallback(onCommand);
    _mqtt.setEnabled(_netCfg.mqttEnabled);
    _mqtt.setBroker(_netCfg.mqttHost, _netCfg.mqttPort);
    _mqtt.setCredentials(_netCfg.mqttUser, _netCfg.mqttPass);
    _mqtt.setHaDiscovery(_netCfg.mqttHaDiscovery);
    if (_netCfg.hmacKey.valid) {
        _mqtt.setCommandHmacKey(_netCfg.hmacKey.bytes, sizeof(_netCfg.hmacKey.bytes));
    }
    if (_netCfg.mqttEnabled) {
        if (!_mqtt.begin()) {
            Serial.printf("[CORE0] MQTT client failed.\n");
        }
    } else {
        Serial.printf("[CORE0] MQTT disabled.\n");
    }

    // ── 12. WebUI ──
    Serial.printf("[CORE0] Starting WebUI...\n");
    _webUi.setAudioProducer(&_audioProducer);
    _webUi.setIntegrations(&_mqtt, &_rtspClient, &_ntp);
    _webUi.setRtspServer(&_rtspServer);
    _webUi.setCommandCallback(onCommand);
    if (!_webUi.begin()) {
        Serial.printf("[CORE0] WebUI failed.\n");
    } else {
        Serial.printf("[CORE0] WebUI listening on :%d\n", WEB_PORT);
    }

    // ── 13. Ethernet ──
#if ETHERNET_ENABLED
    if (_ethManager.begin()) {
        Serial.printf("[CORE0] Ethernet: IP=%s\n",
                      _ethManager.getLocalIP().toString().c_str());
    }
#endif

    if (WiFi.status() == WL_CONNECTED) {
        _mqtt.publishStatus("online");
    }

    // ── 14. System Monitor ──
    _sysMonitor.begin();
    _liveness.reset();

    // TWDT после ensureConnection — портал не должен быть под watchdog.
    (void)esp_task_wdt_deinit();
    if (esp_task_wdt_init(30, true) == ESP_OK) {
        Serial.printf("[CORE0] TWDT timeout=30s\n");
    }

    // ── Готово ──
    printSystemInfo();
    Serial.printf("[CORE0] Initialization complete.\n");
}

// =============================================================================
//  LOOP — основной цикл Core 0 (вызывается FreeRTOS)
// =============================================================================

void loop() {
    TelemetryBuilder::noteLoopTick();
    // ── Captive portal DNS ──
    WiFiSetup::processDNS();

    // ── Мониторинг Wi-Fi ──
    updateWiFiRecovery();

    // Liveness: kick по монотонному счётчику сэмплов (samplesRead за 1с
    // при стабильном потоке повторяется → ложный stall → reboot).
    // При thermal shutdown аудио остановлено законно — watchdog на паузе.
    {
        static bool s_livenessPaused = false;
        if (_sysMonitor.isShutdown()) {
            if (!s_livenessPaused) {
                _liveness.setEnabled(false);
                s_livenessPaused = true;
            }
        } else if (s_livenessPaused) {
            _liveness.reset();
            _liveness.setEnabled(true);
            s_livenessPaused = false;
        }
        static uint32_t s_lastSamples = 0;
        AudioTelemetry telem;
        _audioProducer.getTelemetry(telem);
        if (telem.samplesTotal != s_lastSamples) {
            s_lastSamples = telem.samplesTotal;
            _liveness.kick();
        }
    }
    _liveness.update();

    // ── NTP ресинхронизация (раз в час) ──
    _ntp.update();

    // ── LED update (мигающие паттерны +LEVEL-яркость по RMS) ──
    if (s_ledModeApplied == LED_MODE_STATUS) {
        const bool up = (WiFi.status() == WL_CONNECTED);
        const auto want = up ? LedIndicator::BLINK_NET_OK
                             : LedIndicator::BLINK_NET_FAIL;
        if (_ledIndicator.getPattern() != want) {
            _ledIndicator.setPattern(want);
        }
    }
    _ledIndicator.update();
    if (_ledIndicator.getPattern() == LedIndicator::LEVEL) {
        AudioTelemetry telem;
        _audioProducer.getTelemetry(telem);
        _ledIndicator.setLevel(map(telem.rms, 0, PCM_MAX_AMP, 0, 255));
    }

    // ── MQTT commands; telemetry publishes from sensorPoll ──
    _mqtt.update();

    // ── Ethernet ──
#if ETHERNET_ENABLED
    _ethManager.update();
#endif

    // ── Периодическая статистика (30 сек) ──
    static uint32_t lastStats = 0;
    uint32_t now = millis();
    if (now - lastStats >= 30000) {
        size_t freeHeap  = ESP.getFreeHeap();
        size_t freePsram = ESP.getFreePsram();
        size_t stackCore0 = uxTaskGetStackHighWaterMark(nullptr);
        size_t stackAudio = _audioProducer.getStackHighWaterMark();
        size_t stackRTSP  = _rtspServer.getStackHighWaterMark();
        size_t ringAvail = _audioProducer.available();

        Serial.printf("[CORE0] Stats: Heap=%dKB PSRAM=%dKB | "
                      "Stack: Core0=%d Audio=%d RTSP=%d | RingBuf=%d | "
                      "RTSP-Cli=%d\n",
                      freeHeap / 1024, freePsram / 1024,
                      stackCore0, stackAudio, stackRTSP,
                      ringAvail,
                      _rtspServer.getActiveClientCount());
        lastStats = now;
    }

    // ── Задержка 10 мс — даём время другим задачам Core 0 ──
    TICK_DELAY_MS(10);
}