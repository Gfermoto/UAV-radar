/**
 * @file    MQTTManager.cpp
 * @brief   MQTT: телеметрия, команды, LWT, HA discovery.
 *
 * _pubMutex — mqttTask и mqttCallback; без него гонка connect/publish/disconnect.
 * verifyCommandAuth: fail-closed до NTP (replay после reboot), nonce-cache.
 */

#include "MQTTManager.h"
#include "CommandAuth.h"
#include "TelemetryBuilder.h"
#include "NTPClient.h"
#include "TlsUtil.h"
#include "DiagnosticsNvs.h"
#include <WiFi.h>
#include <cmath>

#define MQTT_TELEMETRY_INTERVAL_MS  1000
#define MQTT_RECONNECT_MIN_MS       2000
#define MQTT_RECONNECT_MAX_MS       30000

MQTTManager *MQTTManager::_instance = nullptr;

MQTTManager::MQTTManager()
    : _mqttClient(_wifiClient)
    , _ntp(nullptr)
    , _port(0)
    , _haDiscovery(true)
    , _enabled(true)
    , _pubMutex(nullptr)
    , _running(false)
    , _reconnectBackoff(MQTT_RECONNECT_MIN_MS, MQTT_RECONNECT_MAX_MS)
    , _commandCallback(nullptr)
    , _cmdHmacKeyLen(0)
    , taskHandle(nullptr)
{
    memset(_cmdHmacKey, 0, sizeof(_cmdHmacKey));
    memset(_host, 0, sizeof(_host));
    memset(_user, 0, sizeof(_user));
    memset(_pass, 0, sizeof(_pass));
    memset(_nodeId, 0, sizeof(_nodeId));
    memset(_telemetryTopic, 0, sizeof(_telemetryTopic));
    memset(_commandTopic, 0, sizeof(_commandTopic));
    memset(_statusTopic, 0, sizeof(_statusTopic));
    memset(_eventsTopic, 0, sizeof(_eventsTopic));
}

bool MQTTManager::configureTls() {
#if CERT_CHAIN_ENABLED
    if (_port != 8883) {
        Serial.printf("[MQTT] WARNING: cleartext MQTT port %u — use only on trusted LAN; prefer 8883+tls_ca\n",
                      (unsigned)_port);
        _tlsReady = false;
        return true;
    }
    if (!TlsUtil::configure(_wifiClient)) {
        Serial.printf("[MQTT] TLS configure failed — cannot connect.\n");
        _tlsReady = false;
        return false;
    }
    _tlsReady = true;
#endif
    return true;
}

void MQTTManager::invalidateTls() {
#if CERT_CHAIN_ENABLED
    _tlsReady = false;
#endif
    if (_pubMutex && xSemaphoreTake(_pubMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        if (_mqttClient.connected()) {
            _mqttClient.disconnect();
        }
        xSemaphoreGive(_pubMutex);
    }
#if CERT_CHAIN_ENABLED
    _wifiClient.stop();
#endif
}

void MQTTManager::setDependencies(NTPClient *ntp) {
    _ntp = ntp;
}

void MQTTManager::setBroker(const char *host, uint16_t port) {
    strncpy(_host, host, sizeof(_host) - 1);
    _port = port;
}

void MQTTManager::setCredentials(const char *user, const char *pass) {
    memset(_user, 0, sizeof(_user));
    memset(_pass, 0, sizeof(_pass));
    if (user) strncpy(_user, user, sizeof(_user) - 1);
    if (pass) strncpy(_pass, pass, sizeof(_pass) - 1);
}

void MQTTManager::setHaDiscovery(bool enabled) {
    _haDiscovery = enabled;
}

void MQTTManager::setEnabled(bool enabled) {
    _enabled = enabled;
    if (!enabled && _pubMutex) {
        if (xSemaphoreTake(_pubMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
            if (_mqttClient.connected()) {
                size_t len = 0;
                char *json = TelemetryBuilder::buildHeartbeatAlloc("offline", &len, nullptr);
                if (json) {
                    _mqttClient.publish(_statusTopic, json, false);
                    free(json);
                }
                _mqttClient.disconnect();
            }
            xSemaphoreGive(_pubMutex);
        }
    }
}

void MQTTManager::applyBrokerSettings(const char *host, uint16_t port,
                                      const char *user, const char *pass, bool haDiscovery) {
    auto apply = [&]() {
        setBroker(host, port);
        setCredentials(user, pass);
        setHaDiscovery(haDiscovery);
        if (!configureTls()) {
            Serial.printf("[MQTT] Broker settings applied but TLS not ready\n");
        }
        _mqttClient.setServer(_host, _port);
        if (_mqttClient.connected()) {
            _mqttClient.disconnect();
        }
    };
    if (_pubMutex) {
        if (xSemaphoreTake(_pubMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
            apply();
            xSemaphoreGive(_pubMutex);
        } else {
            Serial.printf("[MQTT] applyBrokerSettings: mutex timeout\n");
        }
    } else {
        apply();  // pre-begin
    }
}

void MQTTManager::setCommandCallback(MQTTCommandCallback callback) {
    _commandCallback = callback;
}

bool MQTTManager::begin() {
    if (_running) return true;
    if (_taskSync.handle()) return false;

    if (!_ntp) {
        Serial.printf("[MQTT] Dependencies not set\n");
        return false;
    }

    _instance = this;

    // Единственный поток владеет PubSubClient — loop/connect/publish сериализованы.
    _pubMutex = xSemaphoreCreateMutex();
    if (!_pubMutex) {
        Serial.printf("[MQTT] Mutex alloc failed\n");
        return false;
    }

    if (_port == 0) {
        _port = getDefaultPort();
    }

    if (!configureTls()) {
        Serial.printf("[MQTT] TLS required but not configured — abort begin\n");
        vSemaphoreDelete(_pubMutex);
        _pubMutex = nullptr;
        return false;
    }

    uint8_t mac[6];
    WiFi.macAddress(mac);
    snprintf(_nodeId, sizeof(_nodeId), NODE_ID_FMT, mac[3], mac[4], mac[5]);

    snprintf(_telemetryTopic, sizeof(_telemetryTopic), "rtsp-mic/v1/%s/telemetry", _nodeId);
    snprintf(_commandTopic,   sizeof(_commandTopic),   "rtsp-mic/v1/%s/command", _nodeId);
    snprintf(_statusTopic,    sizeof(_statusTopic),    "rtsp-mic/v1/%s/status", _nodeId);
    snprintf(_eventsTopic,    sizeof(_eventsTopic),    "rtsp-mic/v1/%s/events", _nodeId);

    _mqttClient.setServer(_host, _port);
    _mqttClient.setCallback(mqttCallback);
    _mqttClient.setKeepAlive(MQTT_KEEPALIVE_SEC);
    _mqttClient.setBufferSize(2048);

    _running = true;

    if (!_taskSync.prepareStart()) {
        _running = false;
        vSemaphoreDelete(_pubMutex);
        _pubMutex = nullptr;
        return false;
    }

    TaskHandle_t newHandle = nullptr;
    BaseType_t created = xTaskCreatePinnedToCore(
        mqttTask,
        "mqttManager",
        NETWORK_TASK_STACK_SIZE,
        this,
        NETWORK_CONTROL_PRIORITY,
        &newHandle,
        0
    );

    if (created != pdPASS || !newHandle) {
        _running = false;
        _taskSync.abortStart();
        vSemaphoreDelete(_pubMutex);
        _pubMutex = nullptr;
        return false;
    }

    _taskSync.publishHandle(newHandle);
    taskHandle = newHandle;
    _taskSync.releaseTask();
    if (!_taskSync.waitStartup(pdMS_TO_TICKS(TASK_STOP_GRACE_MS * 10))) {
        _running = false;
        _taskSync.requestStop();
        if (_taskSync.handle()) {
            _taskSync.waitExit(pdMS_TO_TICKS(TASK_STOP_GRACE_MS * 10));
        }
        taskHandle = nullptr;
        if (_pubMutex) {  // симметрия с другими fail-путями begin()
            vSemaphoreDelete(_pubMutex);
            _pubMutex = nullptr;
        }
        return false;
    }

    Serial.printf("[MQTT] Client started. Broker: %s:%d TLS=%d NodeID: %s\n",
                  _host, _port, CERT_CHAIN_ENABLED, _nodeId);
    return true;
}

void MQTTManager::stop() {
    _running = false;
    _taskSync.requestStop();
    if (_taskSync.handle()) {
        if (!_taskSync.waitExit(pdMS_TO_TICKS(TASK_STOP_GRACE_MS * 10))) {
            Serial.printf("[MQTT] stop timeout; resources retained\n");
            return;
        }
    }
    taskHandle = nullptr;
    if (_pubMutex) {
        if (xSemaphoreTake(_pubMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
            _mqttClient.disconnect();
            xSemaphoreGive(_pubMutex);
        }
        vSemaphoreDelete(_pubMutex);
        _pubMutex = nullptr;
    }
}

void MQTTManager::setCommandHmacKey(const uint8_t *key, size_t len) {
    _cmdHmacKeyLen = 0;
    memset(_cmdHmacKey, 0, sizeof(_cmdHmacKey));
    if (!key || len == 0) return;
    if (len > sizeof(_cmdHmacKey)) len = sizeof(_cmdHmacKey);
    memcpy(_cmdHmacKey, key, len);
    _cmdHmacKeyLen = len;
}

bool MQTTManager::isConnected() {
    if (!_pubMutex) return false;
    if (xSemaphoreTake(_pubMutex, pdMS_TO_TICKS(20)) != pdTRUE) return false;
    bool ok = _mqttClient.connected();
    xSemaphoreGive(_pubMutex);
    return ok;
}

void MQTTManager::publishTelemetry() {
    if (!_enabled || !_pubMutex) return;

    size_t len = 0;
    char *json = TelemetryBuilder::buildAlloc(false, &len, nullptr);
    if (!json) return;

    if (xSemaphoreTake(_pubMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (_mqttClient.connected()) {
            _mqttClient.publish(_telemetryTopic, json, false);
        }
        xSemaphoreGive(_pubMutex);
    }
    free(json);
}

void MQTTManager::publishStatus(const char *status) {
    if (!_pubMutex || !status) return;

    size_t len = 0;
    char *json = TelemetryBuilder::buildHeartbeatAlloc(status, &len, nullptr);
    if (!json) return;

    if (xSemaphoreTake(_pubMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (_mqttClient.connected()) {
            _mqttClient.publish(_statusTopic, json, false);
        }
        xSemaphoreGive(_pubMutex);
    }
    free(json);
}

void MQTTManager::publishEvent(const char *eventType, const char *details) {
    if (!_enabled || !_pubMutex) return;

    size_t len = 0;
    char *json = TelemetryBuilder::buildEventAlloc(eventType, details, &len, nullptr);
    if (!json) return;

    if (xSemaphoreTake(_pubMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (_mqttClient.connected()) {
            _mqttClient.publish(_eventsTopic, json, false);
        }
        xSemaphoreGive(_pubMutex);
    }
    free(json);
}

void MQTTManager::update() {
    // loop() called only from mqttTask (single thread)
}

bool MQTTManager::reconnectLocked() {
    if (!_enabled) return false;

#if CERT_CHAIN_ENABLED
    if (!_tlsReady && !configureTls()) {
        Serial.printf("[MQTT] TLS not ready — skip reconnect\n");
        _reconnectBackoff.recordFailure(millis());
        return false;
    }
#endif

    uint32_t now = millis();
    if (!_reconnectBackoff.ready(now)) {
        return false;
    }

    bool ok = false;
    // LWT: broker publishes retained "offline" on ungraceful drop (не нужен manual offline).
    {
        static char s_lwtPayload[192];
        snprintf(s_lwtPayload, sizeof(s_lwtPayload),
                 "{\"schema\":\"rtsp-mic.status.v1\",\"node_id\":\"%s\",\"status\":\"offline\",\"lwt\":true}",
                 _nodeId);
        if (_user[0]) {
            ok = _mqttClient.connect(_nodeId, _user, _pass,
                                     _statusTopic, 1, true, s_lwtPayload);
        } else {
            ok = _mqttClient.connect(_nodeId,
                                     _statusTopic, 1, true, s_lwtPayload);
        }
    }
    now = millis();
    if (!ok) {
        int state = _mqttClient.state();
        Serial.printf("[MQTT] Connection failed to %s:%d: state %d\n",
                      _host, _port, state);
        _reconnectBackoff.recordFailure(now);
        return false;
    }
    _reconnectBackoff.recordSuccess();

    if (_mqttClient.subscribe(_commandTopic)) {
        Serial.printf("[MQTT] Subscribed to %s\n", _commandTopic);
    }

    if (_haDiscovery) {
        publishHomeAssistantDiscovery();
    }

    Serial.printf("[MQTT] Connected to broker %s:%d\n", _host, _port);
    size_t hbLen = 0;
    char *json = TelemetryBuilder::buildHeartbeatAlloc("online", &hbLen, nullptr);
    if (json) {
        _mqttClient.publish(_statusTopic, json, true);  // retained — пара к LWT
        free(json);
    }
    return true;
}

bool MQTTManager::wasNonceSeen(const char *nonce) const {
    if (!nonce || !nonce[0]) return false;
    const uint32_t now = millis();
    const size_t nlen = strnlen(nonce, kCmdNonceMax + 1);
    if (nlen == 0 || nlen > kCmdNonceMax) return false;
    for (size_t i = 0; i < kCmdNonceCache; ++i) {
        if (!_cmdSeenNonce[i][0]) continue;
        // Wrap-safe: expired when (now - seenAt) >= ttl (unsigned modular arith).
        const uint32_t seenAt = _cmdSeenNonceAtMs[i];
        if ((uint32_t)(now - seenAt) >= (uint32_t)MQTT_CMD_MAX_AGE_MS + 1000u) {
            continue;
        }
        if (strncmp(_cmdSeenNonce[i], nonce, kCmdNonceMax) == 0) return true;
    }
    return false;
}

bool MQTTManager::rememberNonce(const char *nonce, uint32_t seenAtMs) {
    if (!nonce || !nonce[0]) return false;
    const size_t nlen = strnlen(nonce, kCmdNonceMax + 1);
    if (nlen == 0 || nlen > kCmdNonceMax) return false;
    const uint32_t now = millis();
    const uint32_t ttl = (uint32_t)MQTT_CMD_MAX_AGE_MS + 1000u;
    size_t slot = kCmdNonceCache;
    for (size_t n = 0; n < kCmdNonceCache; ++n) {
        const size_t i = (_cmdSeenNonceIdx + n) % kCmdNonceCache;
        if (!_cmdSeenNonce[i][0] ||
            (uint32_t)(now - _cmdSeenNonceAtMs[i]) >= ttl) {
            slot = i;
            break;
        }
    }
    if (slot >= kCmdNonceCache) return false;  // cache full — fail-closed, не принимать
    strncpy(_cmdSeenNonce[slot], nonce, kCmdNonceMax);
    _cmdSeenNonce[slot][kCmdNonceMax] = '\0';
    _cmdSeenNonceAtMs[slot] = seenAtMs;
    _cmdSeenNonceIdx = (uint8_t)((slot + 1) % kCmdNonceCache);
    return true;
}

bool MQTTManager::verifyCommandAuth(JsonDocument &doc) {
    if (_cmdHmacKeyLen == 0) return false;

    const char *sig = doc["sig"];
    int64_t ts = doc["ts"] | 0;
    const char *cmd = doc["cmd"];
    const char *nonce = doc["nonce"] | "";
    if (!sig || !cmd || ts <= 0) return false;
    if (strlen(sig) != 64) return false;
    // Nonce required (min 8 chars) — closes legacy replay via rotating sig cache.
    const size_t nonceLen = strnlen(nonce, kCmdNonceMax + 1);
    if (nonceLen < 8 || nonceLen > kCmdNonceMax) return false;
    if (wasNonceSeen(nonce)) return false;

    // Reject remote cmds until NTP is synced. Boot-domain millis + RAM nonce
    // cache would allow replay of a captured early-boot command after reboot
    // (nonce table wiped, uptime ts window repeats).
    constexpr int64_t kEpochMsFloor = 1000000000000LL;  // ~2001-09 in ms
    if (!_ntp || !_ntp->isSynced()) return false;
    if (ts < kEpochMsFloor) return false;
    const int64_t nowEpoch = _ntp->getEpochMillis();
    if (llabs((long long)(nowEpoch - ts)) > (long long)MQTT_CMD_MAX_AGE_MS) return false;

    char payload[256];
    if (CommandAuth::buildSignedPayload(payload, sizeof(payload), cmd, ts, nonce,
                                        doc["value"]) == 0) {
        return false;
    }
    if (!CommandAuth::verifyHmac(_cmdHmacKey, _cmdHmacKeyLen, payload, sig)) {
        return false;
    }
    // Accept only if we can retain the nonce (fail closed when cache saturated).
    if (!rememberNonce(nonce, millis())) return false;
    return true;
}

uint16_t MQTTManager::getDefaultPort() const {
    return 1883;
}

// Home Assistant MQTT Auto-Discovery
void MQTTManager::publishHomeAssistantDiscovery() {
    if (!_mqttClient.connected() || !_haDiscovery) return;

    DynamicJsonDocument device(192);
    JsonObject dev = device.to<JsonObject>();
    JsonArray ids = dev["identifiers"].to<JsonArray>();
    const char *haPrefix = "rtsp_mic";
    const char *haMaker = "RTSP Mic";
    ids.add(String(haPrefix) + "_" + _nodeId);
    dev["manufacturer"] = haMaker;
    dev["model"] = NODE_TYPE_STR;
    dev["name"] = String(haMaker) + " " + _nodeId;
    dev["sw_version"] = FIRMWARE_VERSION;

    auto pubSensor = [&](const char *suffix, const char *name, const char *tmpl,
                         const char *unit, const char *icon) {
        String topic = String("homeassistant/sensor/") + haPrefix + "_" + _nodeId + "_" + suffix + "/config";
        DynamicJsonDocument doc(512);
        doc["name"] = String(haMaker) + " " + _nodeId + " " + name;
        doc["state_topic"] = _telemetryTopic;
        doc["value_template"] = tmpl;
        if (unit && unit[0]) doc["unit_of_measurement"] = unit;
        if (icon && icon[0]) doc["icon"] = icon;
        doc["unique_id"] = String(haPrefix) + "_" + _nodeId + "_" + suffix;
        doc["device"] = device.as<JsonObject>();
        char buf[512];
        size_t n = serializeJson(doc, buf, sizeof(buf));
        if (n > 0 && n < sizeof(buf)) {
            _mqttClient.publish(topic.c_str(), buf, true);
        }
    };

    pubSensor("doa", "DOA", "{{ value_json.audio.doa_deg }}", "\u00b0", "mdi:compass");
    pubSensor("peak", "Peak Level", "{{ value_json.audio.peak_level }}", "dBFS", "mdi:microphone");
    pubSensor("rms", "RMS", "{{ value_json.audio.rms_db }}", "dBFS", "mdi:waveform");
    pubSensor("laeq", "LAeq", "{{ value_json.audio.laeq_db }}", "dB", "mdi:volume-high");

    Serial.printf("[MQTT] Home Assistant discovery published for node %s\n", _nodeId);
}

void MQTTManager::mqttCallback(char *topic, byte *payload, unsigned int length) {
    char message[256] = {0};
    size_t copyLen = (length < sizeof(message) - 1) ? length : sizeof(message) - 1;
    memcpy(message, payload, copyLen);

    if (!_instance) return;

    DynamicJsonDocument cmdDoc(256);
    DeserializationError err = deserializeJson(cmdDoc, message);
    if (err) return;

    if (!_instance->verifyCommandAuth(cmdDoc)) {
        Serial.printf("[MQTT] Command rejected: bad signature/nonce/ts\n");
        return;
    }

    const char *cmd = cmdDoc["cmd"];
    if (!cmd) return;
    Serial.printf("[MQTT] Incoming command: %s (topic=%s)\n", cmd, topic);

    if (strcmp(cmd, "reboot") == 0) {
        // Called from loop() under _pubMutex — never take the mutex again here.
        Serial.printf("[MQTT] Reboot requested (deferred offline publish)\n");
        if (_instance) {
            _instance->_pendingOfflineReboot.store(true, std::memory_order_release);
        }
    }
    else if (_instance->_commandCallback) {
        _instance->_commandCallback(cmd, cmdDoc["value"]);
    }
}

void MQTTManager::mqttTask(void *param) {
    MQTTManager *self = static_cast<MQTTManager *>(param);
    self->_taskSync.waitForRelease();
    self->_taskSync.signalStartup(true);

    while (self->_running && !self->_taskSync.stopRequested()) {
        if (xSemaphoreTake(self->_pubMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (!self->_mqttClient.connected()) {
                self->reconnectLocked();
            } else {
                self->_mqttClient.loop();
            }
            xSemaphoreGive(self->_pubMutex);
        }

        if (self->_pendingOfflineReboot.exchange(false, std::memory_order_acq_rel)) {
            Serial.printf("[MQTT] Executing deferred reboot (graceful shutdown)\n");
            self->publishStatus("offline");
            vTaskDelay(pdMS_TO_TICKS(500));
            if (xSemaphoreTake(self->_pubMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
                self->_mqttClient.disconnect();
                xSemaphoreGive(self->_pubMutex);
            }
            DiagnosticsNvs::writeLastEvent("mqtt_reboot", true);
            vTaskDelay(pdMS_TO_TICKS(1000));
#if !defined(UNIT_TEST)
            ESP.restart();
#endif
        }
        TICK_DELAY_MS(50);
    }

    if (xSemaphoreTake(self->_pubMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        self->_mqttClient.disconnect();
        xSemaphoreGive(self->_pubMutex);
    }
    self->_taskSync.signalExit();
    vTaskDelete(nullptr);
}
