/**
 * @file    WiFiRecovery.h
 * @brief   State machine переподключения Wi-Fi с ReconnectBackoff.
 *
 * Фазы: CONNECTED → DISCONNECTED → BACKOFF → CONNECTING → CONNECTED.
 * tick() на Core 0; consumeConnectRequest() — одноразовый запрос WiFi.begin.
 *
 * @see ReconnectBackoff (RtpStreamGuard.h), docs/ARCHITECTURE.md
 */

#ifndef WIFI_RECOVERY_H
#define WIFI_RECOVERY_H

#include <cstdint>
#include "RtpStreamGuard.h"

enum class WiFiRecoveryPhase : uint8_t {
    CONNECTED,
    DISCONNECTED,
    BACKOFF,
    CONNECTING,
};

class WiFiRecovery {
public:
    WiFiRecovery(uint32_t checkIntervalMs, uint32_t connectTimeoutMs)
        : _checkIntervalMs(checkIntervalMs)
        , _connectTimeoutMs(connectTimeoutMs)
        , _backoff(5000, 60000) {}

    WiFiRecoveryPhase phase() const { return _phase; }

    void tick(uint32_t nowMs, bool wifiConnected) {
        if (nowMs - _lastCheckMs < _checkIntervalMs) return;
        _lastCheckMs = nowMs;

        if (_phase == WiFiRecoveryPhase::CONNECTING) {
            if (wifiConnected) {
                _phase = WiFiRecoveryPhase::CONNECTED;
                _backoff.recordSuccess();
                return;
            }
            if (nowMs - _connectStartedMs >= _connectTimeoutMs) {
                _phase = WiFiRecoveryPhase::BACKOFF;
                _backoff.recordFailure(nowMs);
            }
            return;
        }

        if (wifiConnected) {
            if (_phase != WiFiRecoveryPhase::CONNECTED) {
                _backoff.recordSuccess();
            }
            _phase = WiFiRecoveryPhase::CONNECTED;
            return;
        }

        if (_phase == WiFiRecoveryPhase::CONNECTED) {
            _phase = WiFiRecoveryPhase::DISCONNECTED;
            _backoff.recordFailure(nowMs);
            return;
        }

        if (_phase == WiFiRecoveryPhase::DISCONNECTED ||
            _phase == WiFiRecoveryPhase::BACKOFF) {
            if (_backoff.ready(nowMs)) {
                _phase = WiFiRecoveryPhase::CONNECTING;
                _connectStartedMs = nowMs;
                _pendingConnect = true;
            }
        }
    }

    bool consumeConnectRequest() {
        if (!_pendingConnect) return false;
        _pendingConnect = false;
        return true;
    }

private:
    uint32_t _checkIntervalMs;
    uint32_t _connectTimeoutMs;
    WiFiRecoveryPhase _phase = WiFiRecoveryPhase::CONNECTED;
    uint32_t _lastCheckMs = 0;
    uint32_t _connectStartedMs = 0;
    bool _pendingConnect = false;
    ReconnectBackoff _backoff;
};

#endif
