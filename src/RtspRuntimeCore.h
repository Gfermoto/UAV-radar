/**
 * @file    RtspRuntimeCore.h
 * @brief   Общая политика RTSP/RTP для local server и remote client.
 *
 * - ответ control-канала: timeout → reconnect;
 * - interleaved TCP write без retry/delay (не блокировать RTSP-задачу);
 * - RTP timestamp: фиксированный шаг кадра Opus, не wall-clock delta.
 *
 * @see EncodedAudioFanout.h, docs/ARCHITECTURE.md
 */

#ifndef RTSP_RUNTIME_CORE_H
#define RTSP_RUNTIME_CORE_H

#include <cstddef>
#include <cstdint>
#include "RtpStreamGuard.h"

enum class RtspControlResponseAction {
    PROCESS_RESPONSE,
    RECONNECT,
};

/** После wait на control-ответе: есть тело → обработать, иначе переподключиться. */
class RtspControlResponsePolicy {
public:
    static RtspControlResponseAction afterWait(bool hasResponse) {
        return hasResponse ? RtspControlResponseAction::PROCESS_RESPONSE :
                             RtspControlResponseAction::RECONNECT;
    }
};

/**
 * Одна попытка connect + запись failure в backoff.
 * @return true если connect() успешен.
 */
template <typename ConnectFn, typename ClockFn>
bool runRtspConnectAttempt(ConnectFn connect, ClockFn now,
                           ReconnectBackoff &backoff) {
    if (connect()) return true;
    backoff.recordFailure(now());
    return false;
}

/**
 * Полная запись interleaved RTP/RTSP кадра.
 * Short write → false (дыра в одном кадре), без delay/retry — иначе
 * rtsp-задача блокируется и рвёт аудио всем сессиям.
 */
template <typename Transport>
bool writeInterleavedFull(Transport &transport, const uint8_t *data, size_t size) {
    if (!data || size == 0) return false;
    if (!transport.connected()) return false;
    const size_t written = transport.write(data, size);
    return written == size && transport.connected();
}

struct PreparedRtpSend {
    uint16_t sequence;
    uint32_t timestamp;
};

/**
 * Курсор sequence/timestamp RTP для одного потока.
 * Шаг timestamp = frameMs * (clockRate/1000); после pause/resume не использовать
 * разность packetTimestampMs (underflow → «заикание» в VLC).
 */
class RtpSendCursor {
public:
    void reset(uint16_t sequence, uint32_t timestamp, uint32_t lastWriteMs = 0) {
        _nextSequence = sequence;
        _rtpTimestamp = timestamp;
        _lastPacketTimestampMs = 0;
        _lastWriteMs = lastWriteMs;
        _initialized = false;
    }

    /**
     * Подготовить sequence/timestamp для следующего пакета (без мутации состояния).
     * @param clockRate обычно OPUS_RTP_CLOCK_RATE (48000)
     * @param frameMs длительность кадра Opus (20)
     */
    PreparedRtpSend prepare(uint32_t /*packetTimestampMs*/,
                            uint32_t clockRate,
                            uint32_t frameMs = 20) const {
        uint32_t timestamp = _rtpTimestamp;
        if (_initialized) {
            const uint32_t step = frameMs * (clockRate / 1000u);
            timestamp += step;
        }
        return {_nextSequence, timestamp};
    }

    /** Зафиксировать успешную отправку. */
    void commit(uint32_t packetTimestampMs, const PreparedRtpSend &prepared,
                uint32_t nowMs) {
        _nextSequence = static_cast<uint16_t>(prepared.sequence + 1);
        _rtpTimestamp = prepared.timestamp;
        _lastPacketTimestampMs = packetTimestampMs;
        _lastWriteMs = nowMs;
        _initialized = true;
    }

    uint16_t nextSequence() const { return _nextSequence; }
    uint32_t rtpTimestamp() const { return _rtpTimestamp; }
    uint32_t lastWriteMs() const { return _lastWriteMs; }

private:
    uint16_t _nextSequence = 0;
    uint32_t _rtpTimestamp = 0;
    uint32_t _lastPacketTimestampMs = 0;
    uint32_t _lastWriteMs = 0;
    bool _initialized = false;
};

#endif
