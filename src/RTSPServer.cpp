/**
 * @file    RTSPServer.cpp
 * @brief   Реализация локального RTSP-сервера с TCP-interleaved RTP.
 *
 * Accept path: `TcpBudget::tryAcquire` / `release` парно с жизненным циклом
 * сессии. См. `acceptClient`, `closeSession`, `vacateSlot` в acceptClient.
 *
 * @author  RTSPMIC Team
 * @date    2026-07-01
 */

#include "RTSPServer.h"
#include "TcpBudget.h"
#include "WebCredentials.h"

static const char* SDP_TEMPLATE =
    "v=0\r\n"
    "o=- %lu 1 IN IP4 0.0.0.0\r\n"
    "s=RTSPMIC Audio Stream\r\n"
    "t=0 0\r\n"
    "m=audio 0 RTP/AVP 96\r\n"
    "c=IN IP4 0.0.0.0\r\n"
    "a=rtpmap:96 opus/%d/%d\r\n"
    "a=control:trackID=0\r\n"
    "a=fmtp:96 sprop-stereo=0;stereo=0;maxaveragebitrate=%d;useinbandfec=0\r\n";

RTSPServer::RTSPServer()
    : _server(nullptr)
    , _fanout(nullptr)
    , _sessionMutex(nullptr)
    , _running(false)
    , _paused(false)
{}

void RTSPServer::setDependencies(EncodedAudioFanout *fanout) {
    _fanout = fanout;
}

void RTSPServer::reloadCredentials() {
    WebCredentials::load(_authUser, sizeof(_authUser), _authPass, sizeof(_authPass));
}

bool RTSPServer::requireRtspAuth(const String &reqStr, WiFiClient &client, int cseq) {
    String authLine;
    int idx = reqStr.indexOf("Authorization:");
    if (idx < 0) idx = reqStr.indexOf("authorization:");
    if (idx >= 0) {
        int end = reqStr.indexOf("\r\n", idx);
        authLine = (end > idx) ? reqStr.substring(idx, end) : reqStr.substring(idx);
        authLine.trim();
    }
    if (_authUser[0] == '\0' || _authPass[0] == '\0') {
        reloadCredentials();
    }
    if (!WebCredentials::verifyBasicAuth(authLine.c_str(), _authUser, _authPass)) {
        Serial.printf("[RTSP] auth failed\n");
        sendRTSPResponse(client, 401, "Unauthorized",
                         "WWW-Authenticate: Basic realm=\"RTSPMIC\"\r\n",
                         nullptr, cseq);
        return false;
    }
    if (WebCredentials::isDefaultPassword()) {
        Serial.printf("[RTSP] rejected: default password — change via WebUI\n");
        sendRTSPResponse(client, 403, "Forbidden", nullptr, nullptr, cseq);
        return false;
    }
    return true;
}

bool RTSPServer::begin() {
    if (!_fanout) {
        Serial.printf("[RTSP] Ошибка: encoded fan-out не задан\n");
        return false;
    }
    if (_running) return true;
    if (_taskSync.handle()) return false;

    // Сессии мутируют serverTask и stream path — без mutex гонка accept/PLAYING.
    _sessionMutex = xSemaphoreCreateMutex();
    if (!_sessionMutex) {
        Serial.printf("[RTSP] Ошибка создания мьютекса сессий\n");
        return false;
    }

    uint16_t port = getPort();
    _server = new WiFiServer(port, RTSP_MAX_CLIENTS);

    if (!_server) {
        Serial.printf("[RTSP] Ошибка создания WiFiServer\n");
        vSemaphoreDelete(_sessionMutex);
        _sessionMutex = nullptr;
        return false;
    }

    _server->begin();
    _running = true;
    reloadCredentials();

    if (!_taskSync.prepareStart()) {
        _running = false;
        _server->stop();
        delete _server;
        _server = nullptr;
        vSemaphoreDelete(_sessionMutex);
        _sessionMutex = nullptr;
        return false;
    }

    TaskHandle_t newHandle = nullptr;
    BaseType_t created = xTaskCreatePinnedToCore(
        serverTask,
        "rtspServer",
        8192,
        this,
        NETWORK_CONTROL_PRIORITY,
        &newHandle,
        0
    );

    if (created != pdPASS || !newHandle) {
        Serial.printf("[RTSP] Ошибка создания задачи\n");
        _running = false;
        _taskSync.abortStart();
        _server->stop();
        delete _server;
        _server = nullptr;
        vSemaphoreDelete(_sessionMutex);
        _sessionMutex = nullptr;
        return false;
    }
    _taskSync.publishHandle(newHandle);
    _taskSync.releaseTask();
    if (!_taskSync.waitStartup(pdMS_TO_TICKS(TASK_STOP_GRACE_MS * 10))) {
        _running = false;
        _taskSync.requestStop();
        if (_taskSync.handle()) {
            _taskSync.waitExit(pdMS_TO_TICKS(TASK_STOP_GRACE_MS * 10));
        }
        if (_taskSync.canReleaseResources()) {
            _server->stop();
            delete _server;
            _server = nullptr;
            vSemaphoreDelete(_sessionMutex);
            _sessionMutex = nullptr;
        }
        return false;
    }

    Serial.printf("[RTSP] Сервер запущен на порту %d (max клиентов: %d)\n",
                  port, RTSP_MAX_CLIENTS);
    return true;
}

void RTSPServer::stop() {
    _running = false;
    _taskSync.requestStop();

    if (_taskSync.handle()) {
        if (!_taskSync.waitExit(pdMS_TO_TICKS(TASK_STOP_GRACE_MS * 10))) {
            Serial.printf("[RTSP] stop timeout; resources retained\n");
            return;
        }
    }

    if (_server) {
        _server->stop();
        delete _server;
        _server = nullptr;
    }

    if (_sessionMutex) {
        vSemaphoreDelete(_sessionMutex);
        _sessionMutex = nullptr;
    }

    Serial.printf("[RTSP] Сервер остановлен\n");
}

uint8_t RTSPServer::getActiveClientCount() const {
    if (!_sessionMutex ||
        xSemaphoreTake(_sessionMutex, pdMS_TO_TICKS(10)) != pdTRUE) return 0;
    uint8_t count = 0;
    for (int i = 0; i < RTSP_MAX_CLIENTS; i++) {
        if (_sessions[i].state == RTSPSessionState::PLAYING) {
            count++;
        }
    }
    xSemaphoreGive(_sessionMutex);
    return count;
}

uint16_t RTSPServer::getPort() const {
    return rtspLocalPort();
}

void RTSPServer::acceptClient() {
    if (!_server) return;

    // Vacate slot held under mutex: must TcpBudget::release() — иначе leak →
    // budget exhausted и все OPTIONS/RTSP ловят Connection reset.
    auto vacateSlot = [](RTSPSession &slot) {
        slot.client.stop();
        slot.state = RTSPSessionState::IDLE;
        slot.generation++;
        memset(slot.sessionId, 0, sizeof(slot.sessionId));
        slot.rtp.reset(0, 0);
        TcpBudget::release();
    };

    // Не блокировать accept на 250ms: backlog WiFiServer=RTSP_MAX_CLIENTS,
    // иначе burst OPTIONS → Connection reset by peer.
    for (int accepted = 0; accepted < RTSP_MAX_CLIENTS && _server->hasClient();
         ++accepted) {
        WiFiClient newClient = _server->accept();
        if (!newClient) break;

        if (!TcpBudget::tryAcquire()) {
            newClient.stop();
            Serial.printf("[RTSP] TCP budget exhausted, rejecting client\n");
            break;
        }

        if (xSemaphoreTake(_sessionMutex, pdMS_TO_TICKS(50)) != pdTRUE) {
            newClient.stop();
            TcpBudget::release();
            break;
        }

        int freeSlot = -1;
        for (int i = 0; i < RTSP_MAX_CLIENTS; i++) {
            if (_sessions[i].state == RTSPSessionState::IDLE) {
                freeSlot = i;
                break;
            }
        }

        if (freeSlot < 0) {
            const uint32_t now = millis();
            int oldestConnected = -1;
            uint32_t oldestAge = 0;
            for (int i = 0; i < RTSP_MAX_CLIENTS; i++) {
                if (_sessions[i].state == RTSPSessionState::IDLE) continue;
                if (!_sessions[i].client.connected()) {
                    vacateSlot(_sessions[i]);
                    if (freeSlot < 0) freeSlot = i;
                    continue;
                }
                // Только «протухшие» CONNECTED (≥1s): не рвать свежий OPTIONS/DESCRIBE.
                if (_sessions[i].state == RTSPSessionState::CONNECTED) {
                    const uint32_t age = now - _sessions[i].lastKeepAliveMs;
                    if (age >= 1000u && age >= oldestAge) {
                        oldestAge = age;
                        oldestConnected = i;
                    }
                }
            }
            if (freeSlot < 0 && oldestConnected >= 0) {
                vacateSlot(_sessions[oldestConnected]);
                freeSlot = oldestConnected;
            }
        }

        if (freeSlot < 0) {
            xSemaphoreGive(_sessionMutex);
            Serial.printf("[RTSP] Нет свободных слотов. Отказ клиенту %s\n",
                          newClient.remoteIP().toString().c_str());
            newClient.stop();
            TcpBudget::release();
            break;
        }

        RTSPSession &s = _sessions[freeSlot];
        s.client           = newClient;
        s.state            = RTSPSessionState::CONNECTED;
        s.generation++;
        s.rtp.reset(0, 0);
        s.ssrc             = esp_random();
        s.lastKeepAliveMs  = millis();
        snprintf(s.sessionId, sizeof(s.sessionId), "%08lX",
                 static_cast<unsigned long>(s.ssrc));
        const uint32_t ssrc = s.ssrc;
        const uint32_t generation = s.generation;
        const int slot = freeSlot;

        xSemaphoreGive(_sessionMutex);

        Serial.printf("[RTSP] Новый клиент: %s (слот %d, SSRC=0x%08lX)\n",
                      newClient.remoteIP().toString().c_str(), slot,
                      static_cast<unsigned long>(ssrc));

        // Без delay: если кадр уже в сокете — ответим; иначе serverTask дочитает.
        if (newClient.available()) {
            handleRTSP(static_cast<size_t>(slot), generation, newClient);
        }
    }
}

void RTSPServer::handleRTSP(size_t index, uint32_t generation,
                            WiFiClient client) {
    if (!client.connected() || !client.available()) return;

    char request[RTSP_BUF_SIZE] = {0};
    size_t len = 0;

    while (client.available() && len < RTSP_BUF_SIZE - 1) {
        int ch = client.read();
        if (ch < 0) break;
        request[len++] = (char)ch;
        if (len >= 4 && memcmp(request + len - 4, "\r\n\r\n", 4) == 0) break;
    }
    request[len] = '\0';

    if (len == 0) return;

    String reqStr(request);

    // Парсинг CSeq
    int cseq = 0;
    int cseqIdx = reqStr.indexOf("CSeq:");
    if (cseqIdx >= 0) {
        String cseqPart = reqStr.substring(cseqIdx + 5);
        cseqPart.trim();
        int spaceIdx = cseqPart.indexOf('\r');
        if (spaceIdx < 0) spaceIdx = cseqPart.indexOf('\n');
        String numStr = (spaceIdx > 0) ? cseqPart.substring(0, spaceIdx) : cseqPart;
        cseq = numStr.toInt();
    }

    if (reqStr.startsWith("OPTIONS")) {
        const char* extra = "Public: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN, GET_PARAMETER\r\n";
        sendRTSPResponse(client, 200, "OK", extra, nullptr, cseq);
        client.flush();
        // Не закрывать TCP: VLC шлёт OPTIONS→DESCRIBE→SETUP на одном сокете.
        if (xSemaphoreTake(_sessionMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            RTSPSession &session = _sessions[index];
            if (session.generation == generation &&
                session.state != RTSPSessionState::IDLE) {
                session.lastKeepAliveMs = millis();
            }
            xSemaphoreGive(_sessionMutex);
        }
    }
    else if (reqStr.startsWith("DESCRIBE")) {
        if (!requireRtspAuth(reqStr, client, cseq)) return;
        sendSDP(client, cseq);
    }
    else if (reqStr.startsWith("SETUP")) {
        if (!requireRtspAuth(reqStr, client, cseq)) return;
        char sessionId[32] = {};
        bool committed = false;
        if (xSemaphoreTake(_sessionMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            RTSPSession &session = _sessions[index];
            if (session.generation == generation &&
                session.state == RTSPSessionState::CONNECTED) {
                session.rtp.reset((uint16_t)esp_random(), (uint32_t)esp_random());
                session.state = RTSPSessionState::READY;
                session.lastKeepAliveMs = millis();
                strncpy(sessionId, session.sessionId, sizeof(sessionId) - 1);
                committed = true;
            }
            xSemaphoreGive(_sessionMutex);
        }
        if (!committed) return;
        char headers[256];
        snprintf(headers, sizeof(headers),
                 "Transport: RTP/AVP/TCP;interleaved=0-1\r\n"
                 "Session: %s;timeout=60\r\n",
                 sessionId);

        sendRTSPResponse(client, 200, "OK", headers, nullptr, cseq);
        Serial.printf("[RTSP] SETUP: сессия %s\n", sessionId);
    }
    else if (reqStr.startsWith("PLAY")) {
        if (!requireRtspAuth(reqStr, client, cseq)) return;
        char sessionId[32] = {};
        bool ready = false;
        if (xSemaphoreTake(_sessionMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            RTSPSession &session = _sessions[index];
            if (session.generation == generation &&
                session.state == RTSPSessionState::READY) {
                session.state = RTSPSessionState::PLAYING;
                session.lastKeepAliveMs = millis();
                strncpy(sessionId, session.sessionId, sizeof(sessionId) - 1);
                ready = true;
            }
            xSemaphoreGive(_sessionMutex);
        }
        if (ready) {
            char headers[128];
            snprintf(headers, sizeof(headers),
                     "Session: %s\r\n"
                     "Range: npt=0.000-\r\n",
                     sessionId);

            sendRTSPResponse(client, 200, "OK", headers, nullptr, cseq);
        } else {
            sendRTSPResponse(client, 455,
                             "Method Not Valid in This State", nullptr, nullptr, cseq);
        }
    }
    else if (reqStr.startsWith("TEARDOWN")) {
        if (!requireRtspAuth(reqStr, client, cseq)) return;
        sendRTSPResponse(client, 200, "OK", nullptr, nullptr, cseq);
        closeSession(index, generation);
    }
    else if (reqStr.startsWith("GET_PARAMETER")) {
        if (!requireRtspAuth(reqStr, client, cseq)) return;
        if (xSemaphoreTake(_sessionMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            RTSPSession &session = _sessions[index];
            if (session.generation == generation &&
                session.state != RTSPSessionState::IDLE) {
                session.lastKeepAliveMs = millis();
            }
            xSemaphoreGive(_sessionMutex);
        }
        sendRTSPResponse(client, 200, "OK", nullptr, nullptr, cseq);
    }
    else {
        sendRTSPResponse(client, 501, "Not Implemented", nullptr, nullptr, cseq);
    }
}

void RTSPServer::sendRTSPResponse(WiFiClient &client, int code, const char *reason,
                                   const char *extraHeaders, const char *body, int cseq) {
    client.printf("RTSP/1.0 %d %s\r\n", code, reason);
    client.printf("Server: RTSPMIC/%s\r\n", FIRMWARE_VERSION);
    client.printf("CSeq: %d\r\n", cseq);
    client.printf("Date: %s\r\n", __DATE__);

    if (extraHeaders) {
        client.print(extraHeaders);
    }

    if (body) {
        client.printf("Content-Type: application/sdp\r\n");
        client.printf("Content-Length: %d\r\n", strlen(body));
        client.print("\r\n");
        client.print(body);
    } else {
        client.print("\r\n");
    }
    client.flush();
}

void RTSPServer::sendSDP(WiFiClient &client, int cseq) {
    uint32_t sessionId = esp_random();
    char sdpBody[512];
    snprintf(sdpBody, sizeof(sdpBody), SDP_TEMPLATE,
             (unsigned long)sessionId, OPUS_RTP_CLOCK_RATE,
             OPUS_RTP_SDP_CHANNELS, OPUS_BITRATE);

    char extraHeaders[256];
    snprintf(extraHeaders, sizeof(extraHeaders),
             "Content-Base: rtsp://%s:%d/\r\n"
             "Content-Type: application/sdp\r\n"
             "Content-Length: %zu\r\n",
             WiFi.localIP().toString().c_str(), getPort(), strlen(sdpBody));

    client.printf("RTSP/1.0 200 OK\r\n");
    client.printf("Server: RTSPMIC/%s\r\n", FIRMWARE_VERSION);
    client.printf("CSeq: %d\r\n", cseq);
    client.print(extraHeaders);
    client.print("\r\n");
    client.print(sdpBody);
    client.flush();
}

bool RTSPServer::sendRTPPacket(SessionSnapshot &session,
                               const EncodedAudioPacket &packet) {
    if (!session.client.connected()) return false;

    size_t payloadSize = packet.size;
    uint16_t packetLen = RTP_HEADER_SIZE + (uint16_t)payloadSize;

    if (packetLen > RTSP_BUF_SIZE - RTP_INTERLEAVE_PREFIX) {
        return false;
    }

    uint8_t buf[RTSP_BUF_SIZE];
    size_t offset = 0;

    buf[offset++] = RTP_INTERLEAVE_MARKER;
    buf[offset++] = RTP_CHANNEL;
    buf[offset++] = (packetLen >> 8) & 0xFF;
    buf[offset++] = packetLen & 0xFF;

    buf[offset++] = RTP_VERSION;
    buf[offset++] = RTP_PAYLOAD_TYPE;
    PreparedRtpSend prepared =
        session.rtp.prepare(packet.timestampMs, OPUS_RTP_CLOCK_RATE);
    buf[offset++] = (prepared.sequence >> 8) & 0xFF;
    buf[offset++] = prepared.sequence & 0xFF;

    uint32_t ts = prepared.timestamp;
    buf[offset++] = (ts >> 24) & 0xFF;
    buf[offset++] = (ts >> 16) & 0xFF;
    buf[offset++] = (ts >> 8) & 0xFF;
    buf[offset++] = ts & 0xFF;
    uint32_t ssrc = session.ssrc;
    buf[offset++] = (ssrc >> 24) & 0xFF;
    buf[offset++] = (ssrc >> 16) & 0xFF;
    buf[offset++] = (ssrc >> 8) & 0xFF;
    buf[offset++] = ssrc & 0xFF;

    memcpy(buf + offset, packet.data, packet.size);
    offset += packet.size;

    if (!writeInterleavedFull(session.client, buf, offset)) return false;
    // Не flush() на каждый кадр — блокирует TCP и даёт клипинг/дыры в VLC.
    uint32_t now = millis();
    session.rtp.commit(packet.timestampMs, prepared, now);
    session.lastKeepAliveMs = now;
    return true;
}

void RTSPServer::streamToClients() {
    if (!_fanout || _paused) return;

    SessionSnapshot active[RTSP_MAX_CLIENTS];
    size_t activeCount = 0;
    if (xSemaphoreTake(_sessionMutex, pdMS_TO_TICKS(10)) != pdTRUE) return;
    for (int i = 0; i < RTSP_MAX_CLIENTS; i++) {
        if (_sessions[i].state == RTSPSessionState::PLAYING) {
            SessionSnapshot &snapshot = active[activeCount++];
            snapshot.index = static_cast<size_t>(i);
            snapshot.generation = _sessions[i].generation;
            snapshot.client = _sessions[i].client;
            snapshot.state = _sessions[i].state;
            snapshot.ssrc = _sessions[i].ssrc;
            snapshot.rtp = _sessions[i].rtp;
            snapshot.lastKeepAliveMs = _sessions[i].lastKeepAliveMs;
            strncpy(snapshot.sessionId, _sessions[i].sessionId,
                    sizeof(snapshot.sessionId) - 1);
        }
    }
    xSemaphoreGive(_sessionMutex);

    if (activeCount == 0) {
        vTaskDelay(pdMS_TO_TICKS(RTSP_CHUNK_INTERVAL_MS));
        return;
    }

    // Opus frame = 20 мс; раньше тик 32 мс + 1 пакет → систематический underrun.
    int sent = 0;
    EncodedAudioPacket packet{};
    while (sent < 4 && _fanout->pop(EncodedAudioConsumer::LOCAL_RTSP, packet)) {
        for (size_t i = 0; i < activeCount; ++i) {
            SessionSnapshot &snapshot = active[i];
            if (!sendRTPPacket(snapshot, packet)) {
                closeSession(snapshot.index, snapshot.generation);
                continue;
            }
            if (xSemaphoreTake(_sessionMutex, portMAX_DELAY) == pdTRUE) {
                RTSPSession &session = _sessions[snapshot.index];
                if (session.generation == snapshot.generation &&
                    session.state == RTSPSessionState::PLAYING) {
                    session.rtp = snapshot.rtp;
                    session.lastKeepAliveMs = snapshot.lastKeepAliveMs;
                }
                xSemaphoreGive(_sessionMutex);
            }
        }
        ++sent;
    }
    if (sent == 0) {
        TICK_DELAY_MS(RTSP_CHUNK_INTERVAL_MS);
    }
}

void RTSPServer::closeSession(size_t index, uint32_t generation) {
    WiFiClient client;
    bool close = false;
    if (xSemaphoreTake(_sessionMutex, portMAX_DELAY) != pdTRUE) return;
    RTSPSession &session = _sessions[index];
    if (session.generation == generation &&
        session.state != RTSPSessionState::IDLE) {
        client = session.client;
        session.state = RTSPSessionState::IDLE;
        session.client.stop();  // PCB → close немедленно, не ждём переиспользования слота
        session.generation++;
        memset(session.sessionId, 0, sizeof(session.sessionId));
        session.rtp.reset(0, 0);
        close = true;
    }
    xSemaphoreGive(_sessionMutex);
    if (close) client.stop();
    // Слот TcpBudget парный acceptClient::tryAcquire — без release бюджет «течёт».
    if (close) TcpBudget::release();
}

void RTSPServer::pauseAudio() {
    _paused = true;
    if (_fanout) _fanout->clear(EncodedAudioConsumer::LOCAL_RTSP);
}

void RTSPServer::resumeAudio() {
    _paused = false;
}

void RTSPServer::cleanupIdleSessions() {
    const uint32_t now = millis();
    for (int i = 0; i < RTSP_MAX_CLIENTS; i++) {
        uint32_t generation = 0;
        bool shouldClose = false;
        if (xSemaphoreTake(_sessionMutex, pdMS_TO_TICKS(10)) != pdTRUE) continue;
        RTSPSession &session = _sessions[i];
        if (session.state != RTSPSessionState::IDLE) {
            generation = session.generation;
            const uint32_t age = now - session.lastKeepAliveMs;
            // Копия WiFiClient вне мьютекса на S3 часто врёт connected() —
            // проверяем живой сокет только на session.client.
            if (!session.client.connected()) {
                shouldClose = true;
            } else if (session.state == RTSPSessionState::CONNECTED &&
                       age > RTSP_CONNECT_TIMEOUT_MS) {
                // OPTIONS/probe без SETUP — не держим слот 70 с.
                shouldClose = true;
            } else if (age > RTSP_IDLE_TIMEOUT_MS) {
                Serial.printf("[RTSP] Таймаут сессии %s\n", session.sessionId);
                shouldClose = true;
            }
        }
        xSemaphoreGive(_sessionMutex);
        if (shouldClose) {
            closeSession(static_cast<size_t>(i), generation);
        }
    }
}

void RTSPServer::serverTask(void *param) {
    RTSPServer *self = static_cast<RTSPServer *>(param);
    self->_taskSync.waitForRelease();
    self->_taskSync.signalStartup(true);
    uint32_t lastCleanup = 0;

    while (self->_running && !self->_taskSync.stopRequested()) {
        self->acceptClient();

        // Читать с session.client под мьютексом — копия WiFiClient на S3
        // часто врёт available()/connected() и глотает OPTIONS.
        for (int i = 0; i < RTSP_MAX_CLIENTS; i++) {
            uint32_t generation = 0;
            bool doHandle = false;
            WiFiClient client;
            if (xSemaphoreTake(self->_sessionMutex, pdMS_TO_TICKS(5)) != pdTRUE) {
                break;
            }
            RTSPSession &session = self->_sessions[i];
            if (session.state != RTSPSessionState::IDLE &&
                session.client.connected() && session.client.available()) {
                generation = session.generation;
                client = session.client;
                doHandle = true;
            }
            xSemaphoreGive(self->_sessionMutex);
            if (doHandle) {
                self->handleRTSP(static_cast<size_t>(i), generation, client);
            }
        }

        self->streamToClients();

        uint32_t now = millis();
        if (now - lastCleanup >= RTSP_CONNECT_TIMEOUT_MS) {
            self->_stackHighWaterMark.store(
                uxTaskGetStackHighWaterMark(nullptr), std::memory_order_release);
            self->cleanupIdleSessions();
            lastCleanup = now;
        }

        // taskYIELD() недостаточен: при нагрузке HTTP/async_tcp IDLE0 не получает
        // CPU → task_wdt. Минимум 1 ms гарантирует тик IDLE.
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    WiFiClient clients[RTSP_MAX_CLIENTS];
    size_t clientCount = 0;
    if (xSemaphoreTake(self->_sessionMutex, portMAX_DELAY) == pdTRUE) {
        for (int i = 0; i < RTSP_MAX_CLIENTS; i++) {
            if (self->_sessions[i].state != RTSPSessionState::IDLE) {
                clients[clientCount++] = self->_sessions[i].client;
                self->_sessions[i].state = RTSPSessionState::IDLE;
                self->_sessions[i].generation++;
            }
        }
        xSemaphoreGive(self->_sessionMutex);
    }
    for (size_t i = 0; i < clientCount; ++i) {
        clients[i].stop();
        TcpBudget::release();
    }

    self->_taskSync.signalExit();
    vTaskDelete(nullptr);
}
