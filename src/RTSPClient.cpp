/**
 * @file    RTSPClient.cpp
 * @brief   Удалённый RTSP-клиент: RECORD + TCP-interleaved RTP.
 *
 * Core 0, _stateMutex — FSM из clientTask и getState().
 * Exponential backoff при обрыве; fail-closed на не-200 RTSP.
 *
 * @author  RTSPMIC Team
 * @date    2026-07-01
 */

#include "RTSPClient.h"

/** Минимальная задержка переподключения (мс). */
#define RTSP_RECONNECT_MIN_MS   1000

/** Максимальная задержка переподключения (мс). */
#define RTSP_RECONNECT_MAX_MS   30000

/** Интервал отправки аудио-чанков (мс). */
#define RTSP_STREAM_INTERVAL_MS 32

static int s_rtspCseq = 1;

RTSPClient::RTSPClient()
    : _fanout(nullptr)
    , _state(RTSPClientState::DISCONNECTED)
    , _stateMutex(nullptr)
    , _running(false)
    , _paused(false)
    , _port(0)
    , _ssrc(0)
    , _seqNum(0)
    , _rtpTimestamp(0)
    , _lastAudioTimestampMs(0)
    , _audioTimestampInitialized(false)
    , _backoff(RTSP_RECONNECT_MIN_MS, RTSP_RECONNECT_MAX_MS)
{
    memset(_host, 0, sizeof(_host));
    memset(_sessionId, 0, sizeof(_sessionId));
}

void RTSPClient::setDependencies(EncodedAudioFanout *fanout) {
    _fanout = fanout;
}

void RTSPClient::setServer(const char *host, uint16_t port) {
    strncpy(_host, host, sizeof(_host) - 1);
    _port = port;
}

bool RTSPClient::begin() {
    if (!_fanout) {
        Serial.printf("[RTSP-CLI] Ошибка: encoded fan-out не задан\n");
        return false;
    }
    if (_running) return true;
    if (_taskSync.handle()) return false;

    // FSM читается из clientTask и API — без mutex гонка state/streamAudio.
    _stateMutex = xSemaphoreCreateMutex();
    if (!_stateMutex) {
        Serial.printf("[RTSP-CLI] Ошибка создания мьютекса\n");
        return false;
    }

    if (_port == 0) {
        _port = getDefaultPort();
    }

    _ssrc = esp_random();
    _running = true;

    if (!_taskSync.prepareStart()) {
        _running = false;
        vSemaphoreDelete(_stateMutex);
        _stateMutex = nullptr;
        return false;
    }

    TaskHandle_t newHandle = nullptr;
    // Core 0: TCP/RTSP не на Core 1 рядом с I2S.
    BaseType_t created = xTaskCreatePinnedToCore(
        clientTask,
        "rtspClient",
        NETWORK_TASK_STACK_SIZE,
        this,
        NETWORK_CONTROL_PRIORITY,
        &newHandle,
        0
    );

    if (created != pdPASS || !newHandle) {
        _running = false;
        _taskSync.abortStart();
        vSemaphoreDelete(_stateMutex);
        _stateMutex = nullptr;
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
            vSemaphoreDelete(_stateMutex);
            _stateMutex = nullptr;
        }
        return false;
    }

    Serial.printf("[RTSP-CLI] Client started. Server: %s:%d\n",
                  _host[0] ? _host : "(не задан)", _port);
    return true;
}

void RTSPClient::stop() {
    _running = false;
    _taskSync.requestStop();
    if (_taskSync.handle()) {
        if (!_taskSync.waitExit(pdMS_TO_TICKS(TASK_STOP_GRACE_MS * 10))) {
            Serial.printf("[RTSP-CLI] stop timeout; resources retained\n");
            return;
        }
    }
    disconnect();
    if (_stateMutex) {
        vSemaphoreDelete(_stateMutex);
        _stateMutex = nullptr;
    }
}

RTSPClientState RTSPClient::getState() const {
    if (!_stateMutex) return _state;
    xSemaphoreTake(_stateMutex, portMAX_DELAY);
    RTSPClientState s = _state;
    xSemaphoreGive(_stateMutex);
    return s;
}

bool RTSPClient::isStreaming() const {
    return (getState() == RTSPClientState::PLAYING);
}

uint16_t RTSPClient::getDefaultPort() const {
    return RTSP_REMOTE_PORT;
}

// =============================================================================
//  Приватные методы
// =============================================================================

void RTSPClient::sendRTSPRequest(const char *method, const char *extraHeaders) {
    if (!_client.connected()) return;

    _client.printf("%s rtsp://%s:%d/trackID=0 RTSP/1.0\r\n", method, _host, _port);
    _client.printf("CSeq: %d\r\n", s_rtspCseq++);
    _client.printf("User-Agent: RTSPMIC/%s\r\n", FIRMWARE_VERSION);

    if (_sessionId[0]) {
        _client.printf("Session: %s\r\n", _sessionId);
    }

    if (extraHeaders) {
        _client.print(extraHeaders);
    }

    _client.print("\r\n");
    _client.flush();
}

void RTSPClient::sendAnnounce() {
    if (!_client.connected()) return;
    char sdp[384];
    snprintf(sdp, sizeof(sdp),
             "v=0\r\n"
             "o=- %lu 1 IN IP4 %s\r\n"
             "s=RTSPMIC Audio Stream\r\n"
             "t=0 0\r\n"
             "m=audio 0 RTP/AVP %d\r\n"
             "c=IN IP4 0.0.0.0\r\n"
             "a=rtpmap:%d opus/%d/%d\r\n"
             "a=fmtp:%d sprop-stereo=0;stereo=0;maxaveragebitrate=%d;useinbandfec=0\r\n"
             "a=control:trackID=0\r\n",
             (unsigned long)esp_random(), WiFi.localIP().toString().c_str(),
             RTP_PAYLOAD_TYPE, RTP_PAYLOAD_TYPE, OPUS_RTP_CLOCK_RATE,
             OPUS_RTP_SDP_CHANNELS, RTP_PAYLOAD_TYPE, OPUS_BITRATE);
    _client.printf("ANNOUNCE rtsp://%s:%d/ RTSP/1.0\r\n", _host, _port);
    _client.printf("CSeq: %d\r\n", s_rtspCseq++);
    _client.printf("User-Agent: RTSPMIC/%s\r\n", FIRMWARE_VERSION);
    _client.print("Content-Type: application/sdp\r\n");
    _client.printf("Content-Length: %u\r\n\r\n", (unsigned)strlen(sdp));
    _client.print(sdp);
    _client.flush();
}

String RTSPClient::readRTSPResponse(uint32_t timeoutMs) {
    // Неблокирующее чтение с таймаутом и жёстким лимитом (DoS / OOM)
    if (!_client.connected()) return "";

    constexpr size_t kMaxResponseBytes = 4096;
    uint32_t start = millis();
    String response;
    response.reserve(512);

    while (millis() - start < timeoutMs) {
        if (_client.available()) {
            char c = _client.read();
            response += c;
            if (response.length() >= kMaxResponseBytes) {
                Serial.printf("[RTSP] Response truncated at %u bytes\n",
                              (unsigned)kMaxResponseBytes);
                break;
            }
            // Конец заголовков: \r\n\r\n
            int len = response.length();
            if (len >= 4 &&
                response[len - 4] == '\r' && response[len - 3] == '\n' &&
                response[len - 2] == '\r' && response[len - 1] == '\n') {
                break;
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }

        if (!_client.connected()) break;
    }

    return response;
}

void RTSPClient::setRuntimeEnabled(bool enabled) {
    _runtimeEnabled = enabled;
    if (!enabled) {
        disconnect();
    }
}

void RTSPClient::handleConnectionState() {
    if (!_runtimeEnabled) {
        disconnect();
        return;
    }

    RTSPClientState state = getState();
    if (state == RTSPClientState::DISCONNECTED ||
        state == RTSPClientState::ERROR) {

        uint32_t now = millis();
        // Backoff: не долбить NVR при недоступности (1–30 с).
        if (!_backoff.ready(now)) {
            return;
        }

        // Проверка параметров сервера
        if (_host[0] == '\0') {
            vTaskDelay(pdMS_TO_TICKS(RTSP_CONNECT_TIMEOUT_MS));
            return;
        }

        setState(RTSPClientState::CONNECTING);
        Serial.printf("[RTSP-CLI] Connecting to %s:%d...\n", _host, _port);

        bool connected = runRtspConnectAttempt(
            [this]() {
                return _client.connect(_host, _port, RTSP_CONNECT_TIMEOUT_MS);
            },
            []() { return millis(); },
            _backoff);
        if (!connected) {
            Serial.printf("[RTSP-CLI] Connection failed to %s:%d\n", _host, _port);
            _client.stop();
            setState(RTSPClientState::DISCONNECTED);
            return;
        }

        _client.setNoDelay(true);
        sendRTSPRequest("OPTIONS", nullptr);
        setState(RTSPClientState::OPTIONS_SENT);
    }
    // ── Обработка ответов ─────────────────────────────────────────
    else {
        String response = readRTSPResponse(RTSP_RESPONSE_TIMEOUT_MS);

        if (RtspControlResponsePolicy::afterWait(response.length() != 0) ==
            RtspControlResponseAction::RECONNECT) {
            failConnection();
            return;
        }

        // Проверка кода ответа
        bool isOK = (response.indexOf("200 OK") >= 0);
        bool isRedirect = (response.indexOf("301") >= 0 || response.indexOf("302") >= 0);

        if (!isOK && !isRedirect) {
            Serial.printf("[RTSP-CLI] Ошибка ответа сервера\n");
            failConnection();
            return;
        }

        switch (getState()) {
            case RTSPClientState::OPTIONS_SENT:
                sendAnnounce();
                setState(RTSPClientState::ANNOUNCE_SENT);
                break;

            case RTSPClientState::ANNOUNCE_SENT:
                sendRTSPRequest("SETUP",
                                "Transport: RTP/AVP/TCP;interleaved=0-1\r\n");
                setState(RTSPClientState::SETUP_SENT);
                break;

            case RTSPClientState::SETUP_SENT: {
                // Извлекаем session ID из ответа
                int sessionIdx = response.indexOf("Session:");
                if (sessionIdx >= 0) {
                    String sessionLine = response.substring(sessionIdx);
                    int semiIdx = sessionLine.indexOf(';');
                    if (semiIdx > 0) {
                        String sid = sessionLine.substring(8, semiIdx);
                        sid.trim();
                        strncpy(_sessionId, sid.c_str(), sizeof(_sessionId) - 1);
                    } else {
                        String sid = sessionLine.substring(8);
                        sid.trim();
                        sid.replace("\r", "");
                        sid.replace("\n", "");
                        strncpy(_sessionId, sid.c_str(), sizeof(_sessionId) - 1);
                    }
                }

                _seqNum       = (uint16_t)esp_random();
                _rtpTimestamp = (uint32_t)esp_random();
                _lastAudioTimestampMs = 0;
                _audioTimestampInitialized = false;

                sendRTSPRequest("RECORD", "Range: npt=0.000-\r\n");
                setState(RTSPClientState::RECORD_SENT);
                break;
            }

            case RTSPClientState::RECORD_SENT:
                setState(RTSPClientState::PLAYING);
                _backoff.recordSuccess();
                Serial.printf("[RTSP-CLI] Стриминг активен. SSRC=0x%08lX\n", _ssrc);
                break;

            default:
                break;
        }
    }
}

bool RTSPClient::sendRTPPacket(const EncodedAudioPacket &packet) {
    size_t payloadSize = packet.size;
    if (RTP_HEADER_SIZE + (size_t)payloadSize >
        RTSP_BUF_SIZE - RTP_INTERLEAVE_PREFIX) return false;
    uint16_t packetLen = RTP_HEADER_SIZE + (uint16_t)payloadSize;

    uint8_t buf[RTSP_BUF_SIZE];
    size_t offset = 0;

    buf[offset++] = RTP_INTERLEAVE_MARKER;
    buf[offset++] = RTP_CHANNEL;
    buf[offset++] = (packetLen >> 8) & 0xFF;
    buf[offset++] = packetLen & 0xFF;

    buf[offset++] = RTP_VERSION;
    buf[offset++] = RTP_PAYLOAD_TYPE;
    buf[offset++] = (_seqNum >> 8) & 0xFF;
    buf[offset++] = _seqNum & 0xFF;
    _seqNum++;

    if (_audioTimestampInitialized) {
        _rtpTimestamp +=
            (packet.timestampMs - _lastAudioTimestampMs) *
            (OPUS_RTP_CLOCK_RATE / 1000);
    }
    uint32_t ts = _rtpTimestamp;
    buf[offset++] = (ts >> 24) & 0xFF;
    buf[offset++] = (ts >> 16) & 0xFF;
    buf[offset++] = (ts >> 8) & 0xFF;
    buf[offset++] = ts & 0xFF;
    _lastAudioTimestampMs = packet.timestampMs;
    _audioTimestampInitialized = true;

    uint32_t ssrc = _ssrc;
    buf[offset++] = (ssrc >> 24) & 0xFF;
    buf[offset++] = (ssrc >> 16) & 0xFF;
    buf[offset++] = (ssrc >> 8) & 0xFF;
    buf[offset++] = ssrc & 0xFF;

    memcpy(buf + offset, packet.data, packet.size);
    offset += packet.size;

    if (!writeInterleavedFull(_client, buf, offset)) return false;
    _client.flush();
    return true;
}

void RTSPClient::streamAudio() {
    if (!_fanout || _paused) return;

    if (_streamGuard.beforeConsume(_client.connected()) ==
        RtpStreamDecision::DISCONNECT) {
        failConnection();
        return;
    }

    EncodedAudioPacket packet{};
    bool read = _fanout->pop(EncodedAudioConsumer::REMOTE_RTSP, packet);

    if (read) {
        if (!sendRTPPacket(packet)) failConnection();
    }
}

void RTSPClient::disconnect() {
    if (_client.connected()) {
        sendRTSPRequest("TEARDOWN", nullptr);
        _client.stop();
    }
    _sessionId[0] = '\0';
    _seqNum = 0;
    _rtpTimestamp = 0;
    _lastAudioTimestampMs = 0;
    _audioTimestampInitialized = false;
    setState(RTSPClientState::DISCONNECTED);
}

void RTSPClient::failConnection() {
    _client.stop();
    _sessionId[0] = '\0';
    _seqNum = 0;
    _rtpTimestamp = 0;
    _lastAudioTimestampMs = 0;
    _audioTimestampInitialized = false;
    _backoff.recordFailure(millis());
    setState(RTSPClientState::DISCONNECTED);
}

void RTSPClient::pauseAudio() {
    _paused = true;
    // Сброс очереди — иначе после resume шлём устаревшие Opus-кадры.
    if (_fanout) _fanout->clear(EncodedAudioConsumer::REMOTE_RTSP);
}

void RTSPClient::resumeAudio() {
    _paused = false;
}

void RTSPClient::setState(RTSPClientState newState) {
    if (!_stateMutex) {
        _state = newState;
        return;
    }
    xSemaphoreTake(_stateMutex, portMAX_DELAY);
    if (_state == newState) { xSemaphoreGive(_stateMutex); return; }
    _state = newState;
    RTSPClientState logged = _state;
    xSemaphoreGive(_stateMutex);

    const char* stateStr = "UNKNOWN";
    switch (logged) {
        case RTSPClientState::DISCONNECTED: stateStr = "DISCONNECTED"; break;
        case RTSPClientState::CONNECTING:   stateStr = "CONNECTING"; break;
        case RTSPClientState::PLAYING:      stateStr = "PLAYING"; break;
        case RTSPClientState::ERROR:        stateStr = "ERROR"; break;
        default: break;
    }

    Serial.printf("[RTSP-CLI] Состояние: %s\n", stateStr);
}

// =============================================================================
//  Задача FreeRTOS
// =============================================================================

void RTSPClient::clientTask(void *param) {
    RTSPClient *self = static_cast<RTSPClient *>(param);
    self->_taskSync.waitForRelease();
    self->_taskSync.signalStartup(true);

    while (self->_running && !self->_taskSync.stopRequested()) {
        switch (self->getState()) {
            case RTSPClientState::PLAYING:
                self->streamAudio();
                TICK_DELAY_MS(RTSP_STREAM_INTERVAL_MS);
                break;

            case RTSPClientState::CONNECTING:
            case RTSPClientState::OPTIONS_SENT:
            case RTSPClientState::ANNOUNCE_SENT:
            case RTSPClientState::SETUP_SENT:
            case RTSPClientState::RECORD_SENT:
                self->handleConnectionState();
                TICK_DELAY_MS(50);
                break;

            case RTSPClientState::DISCONNECTED:
            case RTSPClientState::ERROR:
                self->handleConnectionState();
                TICK_DELAY_MS(500);
                break;
            default:
                break;
        }

        taskYIELD();
    }

    self->disconnect();
    self->_taskSync.signalExit();
    vTaskDelete(nullptr);
}
