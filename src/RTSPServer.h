/**
 * @file    RTSPServer.h
 * @brief   Локальный RTSP-сервер для прямого доступа к Opus-потоку. Core 0.
 *
 * ## Назначение
 *
 * Минимальный RTSP/1.0 сервер с TCP-interleaved RTP. Принимает до
 * `RTSP_MAX_CLIENTS` одновременных клиентов. Команды: OPTIONS, DESCRIBE,
 * SETUP, PLAY, TEARDOWN, GET_PARAMETER.
 *
 * Аудио поступает из `EncodedAudioFanout` (consumer `LOCAL_RTSP`): Opus mono,
 * `I2S_SAMPLE_RATE` Гц, payload type 96. Порт — `rtspLocalPort()` (:554).
 *
 * ## TcpBudget
 *
 * Каждый accept резервирует слот через `TcpBudget::tryAcquire()` — защита пула
 * lwIP PCB от исчерпания при burst OPTIONS/RTSP. Освобождение (`release()`) при
 * закрытии сессии обязательно; иначе бюджет «течёт» и новые клиенты получают
 * Connection reset. См. `TcpBudget.h`, `docs/ARCHITECTURE.md`.
 *
 * ## Аутентификация
 *
 * HTTP Basic через `WebCredentials` (NVS `web_user`/`web_pass` — те же, что WebUI).
 * Проверка на DESCRIBE/SETUP/PLAY/GET_PARAMETER/TEARDOWN; OPTIONS открыт для
 * discovery. Пароль по умолчанию отклоняется (403). URL:
 * `rtsp://user:pass@host:port/`. После смены пароля в WebUI — `reloadCredentials()`.
 *
 * ## Lifecycle
 *
 * Реализует `AudioLifecycleConsumer`: `pauseAudio()`/`resumeAudio()` для OTA и
 * thermal throttle. Задача `rtspServer` на Core 0 (`NETWORK_CONTROL_PRIORITY`).
 *
 * @see EncodedAudioFanout, WebCredentials, docs/ARCHITECTURE.md
 * @author  RTSPMIC Team
 * @date    2026-07-01
 */

#ifndef RTSP_SERVER_H
#define RTSP_SERVER_H

#include <Arduino.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <atomic>
#include "Config.h"
#include "WebCredentials.h"
#include "AudioLifecycle.h"
#include "EncodedAudioFanout.h"
#include "FreeRtosTaskHandshake.h"
#include "RtspRuntimeCore.h"

/** Состояние RTSP-сессии (конечный автомат на слот). */
enum class RTSPSessionState : uint8_t {
    IDLE,       ///< Слот свободен
    CONNECTED,  ///< TCP принят, ждёт SETUP/PLAY
    READY,      ///< SETUP завершён, не PLAYING
    PLAYING,    ///< Активная отдача RTP
};

/** Один клиентский слот (до RTSP_MAX_CLIENTS). */
struct RTSPSession {
    WiFiClient      client;
    RTSPSessionState state = RTSPSessionState::IDLE;
    uint32_t        generation = 0;   ///< Инкремент при vacate — защита от use-after-close
    uint32_t        ssrc = 0;
    RtpSendCursor   rtp;
    uint32_t        lastKeepAliveMs = 0;
    char            sessionId[32]{};
};

class RTSPServer : public AudioLifecycleConsumer {
public:
    RTSPServer();

    /** @brief Привязка fan-out (обязательно до begin). */
    void setDependencies(EncodedAudioFanout *fanout);

    /**
     * @brief Запуск WiFiServer и задачи `rtspServer` на Core 0.
     * @return false если fan-out не задан или не удалось создать задачу/мьютекс.
     */
    bool begin();

    /** @brief Остановка задачи, закрытие всех сессий и сервера. */
    void stop();

    /**
     * @brief Перечитать web_user/web_pass из NVS (после смены пароля в WebUI).
     */
    void reloadCredentials();

    /** @brief Пауза стриминга: clear LOCAL_RTSP в fan-out (OTA/throttle). */
    void pauseAudio() override;

    /** @brief Возобновление отдачи RTP после pauseAudio. */
    void resumeAudio() override;

    /** @return Число слотов не в состоянии IDLE. */
    uint8_t getActiveClientCount() const;

    /** @return Минимальный остаток стека задачи rtspServer (для диагностики). */
    size_t getStackHighWaterMark() const {
        return _stackHighWaterMark.load(std::memory_order_acquire);
    }

private:
    struct SessionSnapshot {
        size_t index = 0;
        uint32_t generation = 0;
        WiFiClient client;
        RTSPSessionState state = RTSPSessionState::IDLE;
        uint32_t ssrc = 0;
        RtpSendCursor rtp;
        uint32_t lastKeepAliveMs = 0;
        char sessionId[32]{};
    };

    WiFiServer     *_server;
    EncodedAudioFanout *_fanout;
    RTSPSession     _sessions[RTSP_MAX_CLIENTS];
    mutable SemaphoreHandle_t _sessionMutex;
    std::atomic<bool> _running{false};
    std::atomic<bool> _paused{false};
    FreeRtosTaskHandshake _taskSync;
    std::atomic<size_t> _stackHighWaterMark{0};
    char _authUser[WEB_CRED_USER_MAX + 1]{};
    char _authPass[WEB_CRED_PASS_MAX]{};

    uint16_t getPort() const;
    void acceptClient();
    void handleRTSP(size_t index, uint32_t generation, WiFiClient client);
    bool requireRtspAuth(const String &reqStr, WiFiClient &client, int cseq);
    void sendRTSPResponse(WiFiClient &client, int code, const char *reason,
                          const char *extraHeaders, const char *body, int cseq = 0);
    void sendSDP(WiFiClient &client, int cseq = 0);
    bool sendRTPPacket(SessionSnapshot &session,
                       const EncodedAudioPacket &packet);
    void streamToClients();
    void closeSession(size_t index, uint32_t generation);
    void cleanupIdleSessions();
    static void serverTask(void *param);
};

#endif // RTSP_SERVER_H
