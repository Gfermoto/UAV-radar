/**
 * @file    TelemetryBuilder.h
 * @brief   Сборка JSON телеметрии / публичного статуса / MQTT event+heartbeat.
 *
 * Схемы:
 * - `rtsp-mic.telemetry.v1` — полный/extended (WebUI WS, MQTT)
 * - `rtsp-mic.public.v1` — урезанный `/status` при default password
 * - `rtsp-mic.event.v1`, `rtsp-mic.status.v1`
 *
 * Hot path: `*Alloc` + PSRAM allocator (без промежуточного `String`).
 * Setters кэшируют DSP/LED/RTSP state из Core 0; чтение аудио — через
 * `AudioProducer` / `XVF3800_Cache` (seqlock/snapshot).
 *
 * @see docs/API_REFERENCE.md, openapi-webui.yaml
 */

#ifndef TELEMETRY_BUILDER_H
#define TELEMETRY_BUILDER_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include "Config.h"

class AudioProducer;
class XVF3800_Cache;
class NTPClient;
class RTSPClient;

class TelemetryBuilder {
public:
    static void init(AudioProducer *producer, XVF3800_Cache *xvfCache,
                     NTPClient *ntp, RTSPClient *rtspClient, const char *nodeId);

    /** Аллокатор для *Alloc; nullptr → malloc. Освобождать парным free. */
    typedef void *(*AllocFn)(size_t);

    /**
     * Телеметрия микрофона в свежий буфер (schema rtsp-mic.telemetry.v1).
     * @param includeExtended true — поля WebUI/WS
     * @param outLen strlen без NUL (опционально)
     * @return NUL-terminated buffer или nullptr при OOM
     */
    static char *buildAlloc(bool includeExtended, size_t *outLen,
                            AllocFn alloc = nullptr);

    /** Обёртка для тестов/legacy; на hot path — buildAlloc. */
    static String build(bool includeExtended = false);

    static void noteLoopTick();
    static uint32_t loopGen();

    /**
     * Публичный статус после Basic (без MAC/SSID/history).
     * Schema: rtsp-mic.public.v1 — при ещё default password.
     */
    static char *buildLocatorAlloc(size_t *outLen, AllocFn alloc = nullptr);
    static String buildLocator();

    /** MQTT event JSON (rtsp-mic.event.v1). */
    static char *buildEventAlloc(const char *eventType, const char *details,
                                 size_t *outLen, AllocFn alloc = nullptr);
    static String buildEvent(const char *eventType, const char *details = nullptr);

    /** Heartbeat без audio-derived полей (rtsp-mic.status.v1). */
    static char *buildHeartbeatAlloc(const char *status, size_t *outLen,
                                     AllocFn alloc = nullptr);
    static String buildHeartbeat(const char *status);

    /** Смещение калибровки SPL, дБ; 0 = не калиброван. */
    static void setCalibrationOffsetDb(float offsetDb);

    static void setDspAgcState(bool enabled);
    static void setDspLimiterState(bool enabled);
    static void setAecEnvState(uint8_t mode);

    /** DSP state для синхронизации WebUI /status. */
    static void setEchoSuppressionState(bool enabled);
    static void setAsroutState(uint8_t enabled);
    static void setLoudspeakerPresentState(bool present);
    static void setDspMicGainState(float gain);
    /** Полный путь усилений для компенсации SPL/noise (mic, ASROUT, ATTNS). */
    static void setDspPathGains(float micGain, float asroutGain, uint8_t asroutEnabled,
                                uint8_t attnsMode, float attnsNominal, float attnsSlope);
    /** Кэш LedMode для поля `led_mode` (0=on, 1=off). @see docs/LED.md */
    static void setSecurityControls(uint8_t ledMode);
    static uint8_t ledMode();

    static void setLocalRtspClientCount(uint8_t n);
    static void setAudioSetupMode(bool on);
    static bool audioSetupMode();
    static void noteSensorTick();
    static uint32_t sensorGen();

    /** Причина reset + NVS last_event — диагностика /status. */
    static void setResetInfo(int reason, const char *event);
    static int  resetReason();
    static const char *lastEvent();

private:
    static AudioProducer  *_producer;
    static XVF3800_Cache  *_xvfCache;
    static NTPClient      *_ntp;
    static RTSPClient     *_rtspClient;
    static char            _nodeId[NODE_ID_LEN];
};

#endif // TELEMETRY_BUILDER_H
