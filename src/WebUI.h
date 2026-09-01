/**
 * @file    WebUI.h
 * @brief   Многостраничный веб-интерфейс: Dashboard, Network, Audio, System, OTA.
 *
 * ## Назначение
 *
 * Предоставляет полноценный интерфейс управления устройством через браузер:
 *   - **Dashboard** — DOA-компас, аудиометр, VU-бар, MEL preview
 *   - **Network** — Wi-Fi scan/config, Ethernet status
 *   - **Audio** — DSP/AGC/HPF, калибровка SPL
 *   - **System** — heap/uptime, смена пароля, reboot, factory reset, LED
 *   - **Integrations** — MQTT, NTP, remote RTSP push
 *   - **OTA** — загрузка .bin через `/update`
 *
 * SPA отдаётся из PSRAM-кэша (plain + gzip). Телеметрия — HTTP `/status` и
 * WebSocket push (read-only). Все мутации — только authenticated REST.
 *
 * ## Публичный API
 *
 *   - `setAudioProducer` / `setIntegrations` / `setRtspServer` — DI до `begin()`
 *   - `setCommandCallback` — делегирование команд XVF из REST
 *   - `begin()` / `stop()` — AsyncWebServer + webTask на Core 0
 *   - `broadcastTelemetry()` — push JSON в авторизованные WS-клиенты
 *   - `invalidateTelemetryCache()` / `freeTelemetryCache()` — сброс кэша `/status`
 *
 * ## Уровни аутентификации
 *
 * | Уровень            | Методы WebUI              | Назначение                          |
 * |--------------------|---------------------------|-------------------------------------|
 * | Public             | `GET /`, PWA assets       | Dashboard без пароля (preview)      |
 * | Basic              | `requireAuth`             | HTTP Basic (`WebCredentials`)       |
 * | Hardened           | `requireHardenedAuth`     | Basic + запрет default password     |
 * | CSRF               | `requireCsrf`             | Заголовок `X-CSRF-Token`            |
 * | Mutable            | `requireMutable`          | Basic + CSRF + no default pw (POST) |
 * | WS ticket          | `WsTicketAuth` + `/api/ws-ticket` | HMAC-билет, привязка к IP   |
 *
 * WebSocket: после upgrade клиент передаёт ticket; без авторизации push не идёт
 * (`WsTelemetryGate`). Rate-limit и lockout после 5 неудачных Basic — см. Auth TU.
 *
 * ## OTA
 *
 * `POST /update` (multipart .bin): SHA-256, `Update` API, перезагрузка после verify.
 * Состояние `_otaState` (UPLOADING → PENDING_VERIFY → SUCCESS/ABORTED).
 * Требует `requireMutable` (Basic + CSRF + сменённый пароль).
 *
 * ## Спецификация HTTP
 *
 * Полный контракт эндпоинтов: [`openapi-webui.yaml`](../openapi-webui.yaml).
 * NVS, MQTT, auth-матрица: [`docs/API_REFERENCE.md`](../docs/API_REFERENCE.md).
 * Реализация маршрутов — `WebUI_Routes.cpp`; auth — `WebUI_Auth.cpp`.
 *
 * @see WebUI_Internal.h, docs/ARCHITECTURE.md
 * @author  RTSPMIC Team
 * @date    2026-07-01
 */

#ifndef WEBUI_H
#define WEBUI_H

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <AsyncWebSocket.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <ArduinoJson.h>
#include "Config.h"
#include <atomic>
#include <mbedtls/sha256.h>
#include "FreeRtosTaskHandshake.h"
#include "WsTicketAuth.h"

#define CSRF_TOKEN_LEN 33
#define WS_TICKET_LEN  ((int)WsTicketAuth::kTicketLen)
#define WS_TICKET_TTL_MS ((int)WsTicketAuth::kDefaultTtlMs)

class AudioProducer;
class MQTTManager;
class RTSPClient;
class RTSPServer;
class NTPClient;

typedef void (*WebUICommandCallback)(const char *command, const JsonVariant &value);

class WebUI {
public:
    WebUI();
    ~WebUI();

    /** @brief Источник аудио/DOA/MEL для REST и телеметрии. */
    void setAudioProducer(AudioProducer *producer);
    /** @brief MQTT, remote RTSP client, NTP для страницы Integrations. */
    void setIntegrations(MQTTManager *mqtt, RTSPClient *rtsp, NTPClient *ntp = nullptr);
    /** @brief Локальный RTSP server — статус клиентов в телеметрии. */
    void setRtspServer(RTSPServer *rtspServer);
    /** @brief Callback для host-команд XVF из `/api/audio` и интеграций. */
    void setCommandCallback(WebUICommandCallback callback);

    /** @brief Запуск HTTP/WS на `webUIPort()`, webTask warm-up HTML-кэша. */
    bool begin();
    /** @brief Остановка сервера, WS, освобождение кэшей. */
    void stop();
    /** @brief Push телеметрии в авторизованные WS (см. WsTelemetryGate). */
    void broadcastTelemetry();

    TaskHandle_t taskHandle;

private:
    AsyncWebServer  *_server;
    AsyncWebSocket  *_ws;
    AudioProducer   *_audio;
    MQTTManager     *_mqtt;
    RTSPClient      *_rtsp;
    RTSPServer      *_rtspServer;
    NTPClient       *_ntp;
    WebUICommandCallback _commandCallback;

    SemaphoreHandle_t _wsMutex;
    /** Сериализация сборки SPA: webTask warm-up + GET / иначе гонка → hang. */
    SemaphoreHandle_t _htmlMutex{nullptr};
    /** Кэш телеметрии: webTask rebuild/free vs /status read — без mutex UAF. */
    SemaphoreHandle_t _telemMutex{nullptr};
    std::atomic<bool> _running{false};
    /** Кэш телеметрии в PSRAM: строим один раз на sensorGen, не на каждый тик. */
    char       *_cachedTelemetry{nullptr};
    uint16_t    _cachedTelemetryLen{0};
    uint32_t    _cachedTelemetryGen{0};
    std::atomic<uint32_t> _restartAtMs{0};
    char              _csrfToken[CSRF_TOKEN_LEN];
    /** Полная SPA в PSRAM: DRAM String(~100KB) обрезался → UI без <script>. */
    char             *_htmlCache{nullptr};
    size_t            _htmlCacheLen{0};
    /** gzip(SPA) в PSRAM — отдача при Accept-Encoding: gzip. */
    uint8_t          *_htmlGzip{nullptr};
    size_t            _htmlGzipLen{0};
    FreeRtosTaskHandshake _taskSync;

    uint8_t           _wsTicketKey[32]{};
    WsTicketAuth::NonceStore _wsTicketNonces{};
    static constexpr size_t WS_AUTHED_SLOTS = 8;
    uint32_t          _wsAuthedIds[WS_AUTHED_SLOTS]{};
    uint8_t           _wsAuthedCount{0};
    /** Client id'ы, для которых TcpBudget::tryAcquire успел (не release на reject). */
    static constexpr size_t WS_BUDGET_SLOTS = 16;
    uint32_t          _wsBudgetIds[WS_BUDGET_SLOTS]{};
    uint8_t           _wsBudgetCount{0};
    bool noteWsBudget(uint32_t clientId);
    bool takeWsBudget(uint32_t clientId);

    // Per-(IP, path-bucket): общий слот на IP ломал SPA (csrf→mel→ticket → 429).
    static constexpr size_t RATE_SLOTS = 24;
    struct RateSlot {
        uint32_t ip{0};
        uint32_t bucket{0};
        uint32_t lastMs{0};
    };
    RateSlot          _rateSlots[RATE_SLOTS]{};

    static constexpr size_t AUTH_LOCK_SLOTS = 32;
    static constexpr uint8_t AUTH_FAIL_LIMIT = 5;
    static constexpr uint32_t AUTH_LOCKOUT_MS = 60000;
    struct AuthLockSlot {
        uint32_t ip{0};
        uint8_t  fails{0};
        uint32_t lockUntilMs{0};
        uint32_t lastTouchMs{0};
    };
    mutable AuthLockSlot _authLocks[AUTH_LOCK_SLOTS]{};

    void issueWsTicket(uint32_t clientIp, char *out, size_t outLen);
    bool consumeWsTicket(uint32_t clientIp, const char *ticket);
    bool isWsClientAuthed(uint32_t clientId) const;

    enum class OtaPhase : uint8_t {
        IDLE,
        UPLOADING,
        PENDING_VERIFY,
        ABORTED,
        SUCCESS,
    };
    struct OtaState {
        OtaPhase   phase = OtaPhase::IDLE;
        uint32_t   totalBytes = 0;
        uint8_t    sha256[32]{};
        bool       hashValid = false;
    };
    OtaState          _otaState;
    uint32_t          _otaStartMs;
    mbedtls_sha256_context _otaShaCtx;
    bool              _otaHashing;

public:
    /** @brief Сброс generation кэша — пересборка при следующем `/status`/WS. */
    void invalidateTelemetryCache();
    /** @brief Освобождение PSRAM-буфера телеметрии (stop / low memory). */
    void freeTelemetryCache();

private:
    uint16_t getPort() const;
    void setupRoutes();
    bool requireAuth(AsyncWebServerRequest *request) const;
    bool isAuthLocked(uint32_t clientIp) const;
    void noteAuthFailure(uint32_t clientIp) const;
    void noteAuthSuccess(uint32_t clientIp) const;
    /** Auth + reject default password (sensitive reads: ticket/mel/integrations). */
    bool requireHardenedAuth(AsyncWebServerRequest *request) const;
    bool requireWsAuth(AsyncWebSocketClient *client);
    bool authorizeWsClient(AsyncWebSocketClient *client, const char *ticket);
    bool requireCsrf(AsyncWebServerRequest *request) const;
    /** Auth + CSRF + reject default password (mutations/OTA). */
    bool requireMutable(AsyncWebServerRequest *request) const;
    bool validateUrl(const char *url, size_t maxLen) const;
    bool validateHostname(const char *host, size_t maxLen) const;
    bool checkRateLimit(uint32_t clientIp, uint32_t bucket, uint32_t minIntervalMs = 250);
    bool requireRateOk(AsyncWebServerRequest *request, uint32_t minIntervalMs = 250);
    void refreshCsrf();
    void scheduleRestart(uint32_t delayMs = 500);
    String buildHtmlPage() const;
    bool ensureHtmlCache();
    void freeHtmlCache();
    void onWebSocketEvent(AsyncWebSocket *server, AsyncWebSocketClient *client,
                          AwsEventType type, void *arg, uint8_t *data, size_t len);
    static void wsEventHandler(AsyncWebSocket *server, AsyncWebSocketClient *client,
                               AwsEventType type, void *arg, uint8_t *data, size_t len);
    static void webTask(void *param);

    static WebUI *_instance;
};

#endif