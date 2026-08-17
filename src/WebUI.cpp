/**
 * @file    WebUI.cpp
 * @brief   WebUI: lifecycle, webTask, WS broadcast, OTA state machine.
 *
 * Маршруты → `WebUI_Routes.cpp`; auth → `WebUI_Auth.cpp`;
 * HTML-кэш → `WebUI_Html.cpp`; alloc → `WebUI_Alloc.cpp`.
 * HTTP: `openapi-webui.yaml`; NVS/auth: `docs/API_REFERENCE.md`.
 */

#include "WebUI.h"
#include "WebUI_Internal.h"
#include "TcpBudget.h"
#include "WsTelemetryGate.h"
#include "TelemetryBuilder.h"
#include "Config.h"
#include <Preferences.h>
#include <cstring>
#include <esp_heap_caps.h>

WebUI *WebUI::_instance = nullptr;

WebUI::~WebUI() {
    stop();
    freeHtmlCache();
    freeTelemetryCache();
    _instance = nullptr;
}

WebUI::WebUI()
    : _server(nullptr)
    , _ws(nullptr)
    , _audio(nullptr)
    , _mqtt(nullptr)
    , _rtsp(nullptr)
    , _rtspServer(nullptr)
    , _ntp(nullptr)
    , _commandCallback(nullptr)
    , _wsMutex(nullptr)
    , _running(false)
    , _wsAuthedCount(0)
    , _otaHashing(false)
    , taskHandle(nullptr)
{
    for (size_t i = 0; i < sizeof(_wsTicketKey); i += sizeof(uint32_t)) {
        const uint32_t value = esp_random();
        memcpy(_wsTicketKey + i, &value, sizeof(value));
    }
    memset(_csrfToken, 0, sizeof(_csrfToken));
    memset(_rateSlots, 0, sizeof(_rateSlots));
    memset(_authLocks, 0, sizeof(_authLocks));
    _instance = this;
}

void WebUI::setAudioProducer(AudioProducer *producer) { _audio = producer; }
void WebUI::setIntegrations(MQTTManager *mqtt, RTSPClient *rtsp, NTPClient *ntp) {
    _mqtt = mqtt;
    _rtsp = rtsp;
    _ntp  = ntp;
}
void WebUI::setRtspServer(RTSPServer *rtspServer) { _rtspServer = rtspServer; }
void WebUI::setCommandCallback(WebUICommandCallback callback) {
    _commandCallback = callback;
}

bool WebUI::begin() {
    Serial.printf("[WEB] begin() called\n");
    if (_running) {
        Serial.printf("[WEB] already running, returning\n");
        return true;
    }
    if (_taskSync.handle()) {
        Serial.printf("[WEB] task handle exists, degraded mode\n");
        _running = true;
        return true;
    }

    // WS broadcast из webTask и HTTP handlers — сериализация textAll/clients.
    _wsMutex = xSemaphoreCreateMutex();
    if (!_wsMutex) {
        Serial.printf("[WEB] WS mutex create failed\n");
        return false;
    }
    _htmlMutex = xSemaphoreCreateMutex();
    if (!_htmlMutex) {
        Serial.printf("[WEB] HTML mutex create failed\n");
        vSemaphoreDelete(_wsMutex);
        _wsMutex = nullptr;
        return false;
    }
    _telemMutex = xSemaphoreCreateMutex();
    if (!_telemMutex) {
        Serial.printf("[WEB] telem mutex create failed\n");
        vSemaphoreDelete(_htmlMutex); _htmlMutex = nullptr;
        vSemaphoreDelete(_wsMutex); _wsMutex = nullptr;
        return false;
    }
    refreshCsrf();

    uint16_t port = getPort();
    _server = new AsyncWebServer(port);
    _ws     = new AsyncWebSocket(WS_ENDPOINT);

    if (!_server || !_ws) {
        Serial.printf("[WEB] Server/WS alloc failed\n");
        delete _server; _server = nullptr;
        delete _ws; _ws = nullptr;
        vSemaphoreDelete(_telemMutex); _telemMutex = nullptr;
        vSemaphoreDelete(_htmlMutex); _htmlMutex = nullptr;
        vSemaphoreDelete(_wsMutex); _wsMutex = nullptr;
        return false;
    }

    Serial.printf("[WEB] server created on port %u\n", (unsigned)port);

    _ws->onEvent(wsEventHandler);
    _server->addHandler(_ws);

    setupRoutes();
    _server->begin();
    _running = true;
    Serial.printf("[WEB] server->begin() done, port %u open\n", (unsigned)port);

    {
        Preferences prefs;
        prefs.begin("rtspmic", true);
        String pass = prefs.getString("web_pass", WEB_UI_PASSWORD);
        prefs.end();
        if (pass == WEB_UI_DEFAULT_PASSWORD) {
            Serial.printf("[WEB] WARN: default WebUI password — change before use\n");
        }
        Serial.printf("[WEB] WARN: cleartext HTTP on :%u — use TLS reverse-proxy on untrusted LAN\n",
                      (unsigned)port);
    }

    if (!_taskSync.prepareStart()) {
        _running = false;
        _server->end();
        delete _server;
        delete _ws;
        _server = nullptr;
        _ws = nullptr;
        vSemaphoreDelete(_telemMutex);
        _telemMutex = nullptr;
        vSemaphoreDelete(_htmlMutex);
        _htmlMutex = nullptr;
        vSemaphoreDelete(_wsMutex);
        _wsMutex = nullptr;
        return false;
    }

    TaskHandle_t newHandle = nullptr;
    BaseType_t created = xTaskCreatePinnedToCore(
        webTask, "webUI",
        WEB_UI_TASK_STACK_SIZE, this,
        NETWORK_CONTROL_PRIORITY, &newHandle, 0
    );

    if (created != pdPASS || !newHandle) {
        Serial.printf("[WEB] task create FAILED (rc=%d)\n", (int)created);
        _running = false;
        _taskSync.abortStart();
        _server->end();
        delete _server;
        delete _ws;
        _server = nullptr;
        _ws = nullptr;
        vSemaphoreDelete(_telemMutex);
        _telemMutex = nullptr;
        vSemaphoreDelete(_htmlMutex);
        _htmlMutex = nullptr;
        vSemaphoreDelete(_wsMutex);
        _wsMutex = nullptr;
        return false;
    }

    _taskSync.publishHandle(newHandle);
    taskHandle = newHandle;
    _taskSync.releaseTask();
    Serial.printf("[WEB] task released, waiting for startup ack\n");
    if (!_taskSync.waitStartup(pdMS_TO_TICKS(TASK_STOP_GRACE_MS * 10))) {
        Serial.printf("[WEB] task startup timeout — running degraded (WS only)\n");
        _running = true;  // сервер уже слушает, не убиваем его
        _taskSync.signalStartup(true);  // latch running
        taskHandle = newHandle;         // keep handle in case we can signal later
        return true;
    }

    Serial.printf("[WEB] Server port=%u running\n", (unsigned)port);
    return true;
}

void WebUI::stop() {
    _running = false;
    _taskSync.requestStop();
    if (_taskSync.handle()) {
        if (!_taskSync.waitExit(pdMS_TO_TICKS(TASK_STOP_GRACE_MS * 10))) {
            Serial.printf("[WEB] stop timeout; resources retained\n");
            return;
        }
    }
    taskHandle = nullptr;
    if (_server) { _server->end(); delete _server; _server = nullptr; }
    if (_ws)     { delete _ws; _ws = nullptr; }
    freeTelemetryCache();  // до удаления _telemMutex (берёт его внутри)
    if (_telemMutex){ vSemaphoreDelete(_telemMutex); _telemMutex = nullptr; }
    freeHtmlCache();
    if (_htmlMutex){ vSemaphoreDelete(_htmlMutex); _htmlMutex = nullptr; }
    if (_wsMutex){ vSemaphoreDelete(_wsMutex); _wsMutex = nullptr; }
    _instance = nullptr;
}

void WebUI::broadcastTelemetry() {
    if (!_ws || !_running) return;

#if defined(ESP_PLATFORM) || defined(ARDUINO)
    const uint32_t freeHeap = ESP.getFreeHeap();
#else
    const uint32_t freeHeap = WS_TELEM_MIN_HEAP;
#endif
    static uint32_t s_lastLowHeapLogMs = 0;
    if (freeHeap < WS_TELEM_MIN_HEAP) {
        if ((millis() - s_lastLowHeapLogMs) > 5000) {
            Serial.printf("[WEB] skip telemetry: low heap=%u\n", (unsigned)freeHeap);
            s_lastLowHeapLogMs = millis();
        }
        return;
    }

    // Перестраиваем JSON только при смене sensorGen (I2S не тикает → нет новых данных).
    // Весь блок под _telemMutex: /status (AsyncTCP task) читает и free'ит тот же кэш.
    if (xSemaphoreTake(_telemMutex, pdMS_TO_TICKS(200)) != pdTRUE) return;
    const uint32_t gen = TelemetryBuilder::sensorGen();
    if (_cachedTelemetryGen != gen) {
        size_t jsLen = 0;
        char *ps = TelemetryBuilder::buildAlloc(true, &jsLen, webUiAlloc);
        if (ps && jsLen > 0 && jsLen <= UINT16_MAX) {
            if (_cachedTelemetry) webUiFree(_cachedTelemetry);
            _cachedTelemetry = ps;
            _cachedTelemetryLen = static_cast<uint16_t>(jsLen);
            _cachedTelemetryGen = gen;
        } else if (ps) {
            webUiFree(ps);
        }
    }
    const size_t payloadLen = _cachedTelemetry ? _cachedTelemetryLen : 0;
    const bool cacheOk = payloadLen > 0 && payloadLen <= WS_TELEM_MAX_PAYLOAD;

    uint16_t sent = 0;
    uint16_t skippedBp = 0;
    uint16_t skippedAuth = 0;
    uint16_t clients = 0;
    if (cacheOk && xSemaphoreTake(_wsMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        _ws->cleanupClients();
        for (auto *client : _ws->getClients()) {
            if (!client) continue;
            ++clients;
            const bool authed = isWsClientAuthed(client->id());
            const WsTelemetryDecision d = wsTelemetryDecide(
                true, authed, client->canSend(), client->queueIsFull(),
                freeHeap, payloadLen);
            if (d.skipUnauthed) {
                ++skippedAuth;
                continue;
            }
            if (!d.send) {
                ++skippedBp;
                continue;
            }
            client->text(_cachedTelemetry, payloadLen);
            ++sent;
        }
        xSemaphoreGive(_wsMutex);
    }
    xSemaphoreGive(_telemMutex);

    static uint32_t s_lastDiagMs = 0;
    if (skippedBp > 0 && (millis() - s_lastDiagMs) > 5000) {
        Serial.printf("[WEB] ws telem sent=%u skip_bp=%u skip_auth=%u clients=%u bytes=%u heap=%u\n",
                      sent, skippedBp, skippedAuth, clients,
                      (unsigned)payloadLen, (unsigned)freeHeap);
        s_lastDiagMs = millis();
    }
}

uint16_t WebUI::getPort() const {
    return WEB_PORT;
}

void WebUI::freeTelemetryCache() {
    if (_telemMutex &&
        xSemaphoreTake(_telemMutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
        return;  // dtor/stop: лучше утечка буфера, чем free под чужим чтением
    }
    if (_cachedTelemetry) {
        webUiFree(_cachedTelemetry);
        _cachedTelemetry = nullptr;
        _cachedTelemetryLen = 0;
        _cachedTelemetryGen = 0;
    }
    if (_telemMutex) xSemaphoreGive(_telemMutex);
}

void WebUI::invalidateTelemetryCache() {
    _cachedTelemetryGen = 0;
}

void WebUI::wsEventHandler(AsyncWebSocket *server, AsyncWebSocketClient *client,
                            AwsEventType type, void *arg, uint8_t *data, size_t len) {
    if (_instance) {
        _instance->onWebSocketEvent(server, client, type, arg, data, len);
    }
}

bool WebUI::noteWsBudget(uint32_t clientId) {
    if (!_wsMutex || xSemaphoreTake(_wsMutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        return false;
    }
    bool ok = false;
    if (_wsBudgetCount < WS_BUDGET_SLOTS) {
        _wsBudgetIds[_wsBudgetCount++] = clientId;
        ok = true;
    }
    xSemaphoreGive(_wsMutex);
    return ok;
}

bool WebUI::takeWsBudget(uint32_t clientId) {
    if (!_wsMutex || xSemaphoreTake(_wsMutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        return false;
    }
    bool found = false;
    for (uint8_t i = 0; i < _wsBudgetCount; ++i) {
        if (_wsBudgetIds[i] == clientId) {
            memmove(&_wsBudgetIds[i], &_wsBudgetIds[i + 1],
                    (_wsBudgetCount - i - 1) * sizeof(_wsBudgetIds[0]));
            _wsBudgetCount--;
            found = true;
            break;
        }
    }
    xSemaphoreGive(_wsMutex);
    return found;
}

void WebUI::onWebSocketEvent(AsyncWebSocket *server, AsyncWebSocketClient *client,
                              AwsEventType type, void *arg, uint8_t *data, size_t len) {
    (void)server;
    (void)data;
    (void)len;
    switch (type) {
        case WS_EVT_CONNECT: {
            if (ESP.getFreeHeap() < WEBUI_GUARD_MIN_HEAP) {
                client->close();
                Serial.printf("[WEB] WS #%u rejected (low heap=%u)\n",
                              client->id(), (unsigned)ESP.getFreeHeap());
                break;
            }
            if (!TcpBudget::tryAcquire()) {
                client->close();
                Serial.printf("[WEB] WS #%u rejected (TCP budget)\n", client->id());
                break;
            }
            // DISCONNECT всегда приходит после close — release только если note ок.
            if (!noteWsBudget(client->id())) {
                TcpBudget::release();
                client->close();
                Serial.printf("[WEB] WS #%u rejected (budget slot full)\n", client->id());
                break;
            }
            Serial.printf("[WEB] WS #%u pending auth (TCP budget used=%d)\n",
                          client->id(), (int)TcpBudget::activeCount());
            break;
        }
        case WS_EVT_DISCONNECT:
            if (_wsMutex && xSemaphoreTake(_wsMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                for (uint8_t i = 0; i < _wsAuthedCount; ++i) {
                    if (_wsAuthedIds[i] == client->id()) {
                        memmove(&_wsAuthedIds[i], &_wsAuthedIds[i + 1],
                                (_wsAuthedCount - i - 1) * sizeof(_wsAuthedIds[0]));
                        _wsAuthedCount--;
                        break;
                    }
                }
                xSemaphoreGive(_wsMutex);
            }
            if (takeWsBudget(client->id())) {
                TcpBudget::release();
            }
            Serial.printf("[WEB] WS #%u disconnected (TCP budget used=%d)\n",
                          client->id(), (int)TcpBudget::activeCount());
            break;
        case WS_EVT_DATA: {
            // Already authenticated clients: ignore further DATA (WS is push-only).
            if (requireWsAuth(client)) break;
            StaticJsonDocument<96> doc;
            if (deserializeJson(doc, data, len) != DeserializationError::Ok) {
                client->close();
                break;
            }
            const char *ticket = doc["auth"] | "";
            if (!authorizeWsClient(client, ticket)) {
                client->close();
                break;
            }
            Serial.printf("[WEB] WS #%u authenticated\n", client->id());
            break;
        }
        default:
            break;
    }
}

void WebUI::webTask(void *param) {
    WebUI *self = static_cast<WebUI *>(param);
    Serial.printf("[WEB] webTask started, waiting for release\n");
    self->_taskSync.waitForRelease();
    Serial.printf("[WEB] webTask released, signaling startup\n");
    self->_taskSync.signalStartup(true);

    // Warm SPA после listen: в begin() до _server->begin() сборка кэша
    // могла зависнуть/сорвать setup → порт 80 не открывался (Connection refused).
    if (!self->ensureHtmlCache()) {
        Serial.printf("[WEB] WARN: SPA cache warm-up failed\n");
    }

    uint32_t lastBroadcast = 0;
    uint32_t lastCsrf = 0;

    while (self->_running && !self->_taskSync.stopRequested()) {
        uint32_t now = millis();

        const uint32_t restartAt = self->_restartAtMs.load();
        if (restartAt != 0 && (int32_t)(now - restartAt) >= 0) {
            ESP.restart();
        }

        if (now - lastBroadcast >= WS_UPDATE_INTERVAL_MS) {
            self->broadcastTelemetry();
            lastBroadcast = now;
        }

        if (now - lastCsrf >= CSRF_REFRESH_MS) {
            self->refreshCsrf();
            lastCsrf = now;
        }

        if (self->_ws) {
            if (xSemaphoreTake(self->_wsMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                self->_ws->cleanupClients();
                xSemaphoreGive(self->_wsMutex);
            }
        }

        TICK_DELAY_MS(100);
    }

    self->_taskSync.signalExit();
    vTaskDelete(nullptr);
}