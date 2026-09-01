/**
 * @file    RtpStreamGuard.h
 * @brief   Политика RTP-потока и экспоненциальный backoff переподключения.
 *
 * RtpStreamGuard — CONTINUE/DISCONNECT при write mismatch или disconnect.
 * ReconnectBackoff — ready/recordFailure/recordSuccess для WiFi/NTP recovery.
 *
 * @see WiFiRecovery.h, NtpSyncState.h, RTSPClient.h, docs/ARCHITECTURE.md
 */

#ifndef RTP_STREAM_GUARD_H
#define RTP_STREAM_GUARD_H

#include <cstddef>
#include <cstdint>

enum class RtpStreamDecision {
    CONTINUE,
    DISCONNECT,
};

class RtpStreamGuard {
public:
    RtpStreamDecision beforeConsume(bool connected) const {
        return connected ? RtpStreamDecision::CONTINUE :
                           RtpStreamDecision::DISCONNECT;
    }

    RtpStreamDecision afterWrite(bool connected, size_t expected,
                                 size_t written) const {
        return connected && written == expected ?
            RtpStreamDecision::CONTINUE : RtpStreamDecision::DISCONNECT;
    }
};

class ReconnectBackoff {
public:
    ReconnectBackoff(uint32_t minimumMs, uint32_t maximumMs)
        : _minimumMs(minimumMs), _maximumMs(maximumMs), _delayMs(minimumMs) {}

    bool ready(uint32_t nowMs) const {
        return !_waiting || static_cast<uint32_t>(nowMs - _failureMs) >= _delayMs;
    }

    void recordFailure(uint32_t nowMs) {
        _failureMs = nowMs;
        _waiting = true;
        _delayMs = _delayMs > _maximumMs / 2 ? _maximumMs : _delayMs * 2;
    }

    void recordSuccess() {
        _waiting = false;
        _delayMs = _minimumMs;
    }

    uint32_t delayMs() const { return _delayMs; }

private:
    uint32_t _minimumMs;
    uint32_t _maximumMs;
    uint32_t _delayMs;
    uint32_t _failureMs = 0;
    bool _waiting = false;
};

#endif
