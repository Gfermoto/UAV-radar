/**
 * @file    MQTTManager.h
 * @brief   MQTT-клиент: телеметрия, статус, события и подписанные команды. Core 0.
 *
 * Топики (Config.h, `{node_id}` подставляется в begin):
 * - publish:  …/telemetry, …/status, …/events
 * - subscribe: …/command → JSON → verify HMAC → CommandDispatcher
 *
 * Auth команд: HMAC-SHA256 + ts + optional nonce (CommandAuth.h).
 * Пустой hmac_key → все команды отклоняются. До NTP — fail-closed.
 *
 * Вызовы: `begin()` поднимает задачу mqttTask; `publishTelemetry()` /
 * `update()` — из loop/sensorPoll на Core 0. Не вызывать с Core 1.
 *
 * @see docs/API_REFERENCE.md §3–4, docs/ARCHITECTURE.md
 */

#ifndef MQTT_MANAGER_H
#define MQTT_MANAGER_H

#include <Arduino.h>
#include <PubSubClient.h>
#if CERT_CHAIN_ENABLED
#include <WiFiClientSecure.h>
#else
#include <WiFiClient.h>
#endif
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <ArduinoJson.h>
#include "Config.h"
#include <atomic>
#include "RtpStreamGuard.h"
#include "FreeRtosTaskHandshake.h"

class NTPClient;

/** Колбэк внешней обработки команд (обычно CommandDispatcher::dispatch). */
typedef void (*MQTTCommandCallback)(const char *command, const JsonVariant &value);

class MQTTManager {
public:
    MQTTManager();
    void setDependencies(NTPClient *ntp);

    void setBroker(const char *host, uint16_t port);

    /** MQTT username/password брокера (пустые = без broker auth). */
    void setCredentials(const char *user, const char *pass);

    void setHaDiscovery(bool enabled);

    /** Новые настройки брокера + переподключение. */
    void applyBrokerSettings(const char *host, uint16_t port,
                             const char *user, const char *pass, bool haDiscovery);

    void setCommandCallback(MQTTCommandCallback callback);

    /** Ключ подписи команд; пустой → reject all. */
    void setCommandHmacKey(const uint8_t *key, size_t len);

    /** Сброс TLS-сессии после смены CA. */
    void invalidateTls();

    bool begin();
    void stop();
    bool isConnected();

    /** false = нет MQTT-трафика (публикации/reconnect). */
    void setEnabled(bool enabled);
    bool isEnabled() const { return _enabled; }
    bool isActive() const { return _running; }

    /** Телеметрия (~1 с из sensorPoll / loop, Core 0). */
    void publishTelemetry();
    void publishStatus(const char *status);
    void publishEvent(const char *eventType, const char *details);

    /** Входящие сообщения / reconnect tick — из main loop. */
    void update();

    TaskHandle_t taskHandle;

private:
#if CERT_CHAIN_ENABLED
    WiFiClientSecure _wifiClient;
#else
    WiFiClient       _wifiClient;
#endif
    PubSubClient   _mqttClient;
    NTPClient     *_ntp;

    char     _host[64];
    uint16_t _port;
    char     _user[32];
    char     _pass[64];
    bool     _haDiscovery;
    bool     _enabled;
    char     _nodeId[NODE_ID_LEN];
    char     _telemetryTopic[64];
    char     _commandTopic[64];
    char     _statusTopic[64];
    char     _eventsTopic[64];

    SemaphoreHandle_t _pubMutex;
    static MQTTManager *_instance;  ///< для статического mqttCallback
    std::atomic<bool> _running{false};
    /** Из mqttCallback под _pubMutex; обрабатывается после Give в mqttTask. */
    std::atomic<bool> _pendingOfflineReboot{false};
    ReconnectBackoff  _reconnectBackoff;
    MQTTCommandCallback _commandCallback;
    uint8_t _cmdHmacKey[32];
    size_t  _cmdHmacKeyLen;
    /** Недавно принятые nonce (anti-replay в окне возраста). */
    static constexpr size_t kCmdNonceCache = 64;
    static constexpr size_t kCmdNonceMax = 32;
    char     _cmdSeenNonce[kCmdNonceCache][kCmdNonceMax + 1]{};
    uint32_t _cmdSeenNonceAtMs[kCmdNonceCache]{};
    uint8_t  _cmdSeenNonceIdx = 0;
    FreeRtosTaskHandshake _taskSync;

    bool reconnectLocked();
    bool verifyCommandAuth(JsonDocument &doc);
    bool wasNonceSeen(const char *nonce) const;
    /** false если кэш насыщен неистёкшими nonce (fail closed). */
    bool rememberNonce(const char *nonce, uint32_t seenAtMs);

#if defined(UNIT_TEST)
public:
    bool testOnly_wasNonceSeen(const char *nonce) const { return wasNonceSeen(nonce); }
    bool testOnly_rememberNonce(const char *nonce, uint32_t seenAtMs) {
        return rememberNonce(nonce, seenAtMs);
    }
    bool testOnly_verifyCommandAuth(JsonDocument &doc) { return verifyCommandAuth(doc); }
    void testOnly_setHmacKey(const uint8_t *key, size_t len) {
        _cmdHmacKeyLen = len;
        memcpy(_cmdHmacKey, key, len);
    }
    void testOnly_setNtp(NTPClient *ntp) { _ntp = ntp; }
private:
#endif

    static void mqttCallback(char *topic, byte *payload, unsigned int length);
    void publishHomeAssistantDiscovery();
    static void mqttTask(void *param);
    uint16_t getDefaultPort() const;
    /** @return false если CERT_CHAIN обязателен, но не готов. */
    bool configureTls();
#if CERT_CHAIN_ENABLED
    bool _tlsReady{false};
#endif
};

#endif // MQTT_MANAGER_H
