/**
 * @file    RTSPClient.h
 * @brief   Необязательный RTSP-клиент: push Opus на внешний рекордер/NVR. Core 0.
 *
 * ## Назначение
 *
 * Удалённый push-путь аудио: подключается к настроенному RTSP-приёмнику
 * (ANNOUNCE/SETUP/RECORD) и шлёт RTP TCP-interleaved. Источник — тот же
 * `EncodedAudioFanout`, consumer **`REMOTE_RTSP`** (параллельно с локальным
 * `RTSPServer` / `LOCAL_RTSP`). Политика drop-oldest в fan-out общая.
 *
 * Автопереподключение с `ReconnectBackoff` при обрыве. Включается/выключается
 * runtime через WebUI (`setRuntimeEnabled` / NetConfig soft-apply).
 *
 * ## Lifecycle
 *
 * `AudioLifecycleConsumer`: pause/stop согласованы с `AudioLifecycle` (OTA,
 * thermal). Задача `rtspClient` на Core 0.
 *
 * @see EncodedAudioFanout, RTSPServer, docs/ARCHITECTURE.md
 * @author  RTSPMIC Team
 * @date    2026-07-01
 */

#ifndef RTSP_CLIENT_H
#define RTSP_CLIENT_H

#include <Arduino.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <atomic>
#include "Config.h"
#include "AudioLifecycle.h"
#include "EncodedAudioFanout.h"
#include "RtpStreamGuard.h"
#include "FreeRtosTaskHandshake.h"
#include "RtspRuntimeCore.h"

/**
 * @brief  Состояние RTSP-клиента (конечный автомат push-сессии).
 */
enum class RTSPClientState : uint8_t {
    DISCONNECTED,   ///< Не подключён к серверу
    CONNECTING,     ///< В процессе подключения
    OPTIONS_SENT,   ///< OPTIONS отправлен, ждём ответ
    ANNOUNCE_SENT,  ///< ANNOUNCE с Opus SDP отправлен
    SETUP_SENT,     ///< SETUP отправлен, ждём Transport
    RECORD_SENT,    ///< RECORD отправлен, ждём OK
    PLAYING,        ///< Активный стриминг
    ERROR           ///< Ошибка, ожидание переподключения
};

class RTSPClient : public AudioLifecycleConsumer {
public:
    RTSPClient();

    /** @brief Привязка fan-out (обязательно до begin). */
    void setDependencies(EncodedAudioFanout *fanout);

    /**
     * @brief  Установка хоста и порта внешнего RTSP-приёмника.
     * @param  host  IP-адрес или хостнейм сервера.
     * @param  port  Порт RTSP-сервера.
     */
    void setServer(const char *host, uint16_t port);

    /** @brief Запуск задачи клиента на Core 0. */
    bool begin();

    /** @brief Остановка клиента и разрыв сессии. */
    void stop();

    /** @brief Пауза: clear REMOTE_RTSP в fan-out. */
    void pauseAudio() override;

    /** @brief Возобновление push после pauseAudio. */
    void resumeAudio() override;

    /** @return Текущее состояние конечного автомата. */
    RTSPClientState getState() const;

    /** @return true если подключён и стримит (PLAYING). */
    bool isStreaming() const;

    /** @brief Runtime enable (soft apply через WebUI / NetConfig). */
    void setRuntimeEnabled(bool enabled);
    bool isRuntimeEnabled() const { return _runtimeEnabled; }
    bool isActive() const { return _running.load(); }

private:
    EncodedAudioFanout *_fanout;
    WiFiClient      _client;
    RTSPClientState _state;
    mutable SemaphoreHandle_t _stateMutex;
    std::atomic<bool> _running{false};
    std::atomic<bool> _paused{false};
    std::atomic<bool> _runtimeEnabled{true};

    char     _host[64];
    uint16_t _port;
    uint32_t _ssrc;
    uint16_t _seqNum;
    uint32_t _rtpTimestamp;
    uint32_t _lastAudioTimestampMs;
    bool     _audioTimestampInitialized;
    char     _sessionId[32];
    RtpStreamGuard _streamGuard;
    ReconnectBackoff _backoff;
    FreeRtosTaskHandshake _taskSync;

    /** Отправка RTSP-запроса на сервер. */
    void sendRTSPRequest(const char *method, const char *extraHeaders);
    void sendAnnounce();

    /** Чтение RTSP-ответа от сервера. */
    String readRTSPResponse(uint32_t timeoutMs = 3000);

    /** Обработка конечного автомата подключения. */
    void handleConnectionState();

    /** Отправка RTP-пакета (TCP-interleaved). */
    bool sendRTPPacket(const EncodedAudioPacket &packet);

    /** Стриминг аудио-чанка из fan-out (REMOTE_RTSP). */
    void streamAudio();

    /** Отключение с очисткой состояния. */
    void disconnect();
    void failConnection();

    /** Переход в указанное состояние с логированием. */
    void setState(RTSPClientState newState);

    /** Получить текущий порт в зависимости от версии прошивки. */
    uint16_t getDefaultPort() const;

    static void clientTask(void *param);
};

#endif // RTSP_CLIENT_H
