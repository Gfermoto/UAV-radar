/**
 * @file    NtpSyncState.h
 * @brief   State machine SNTP: sync, resync, recovery с backoff.
 *
 * Фазы: IDLE/SYNCING → SYNCED | FAILED. tick() — timeout, периодический resync,
 * recovery после FAILED. syncGeneration++ при каждом requestSync.
 *
 * @see NTPClient.h, ReconnectBackoff, docs/ARCHITECTURE.md
 */

#ifndef NTP_SYNC_STATE_H
#define NTP_SYNC_STATE_H

#include <cstdint>
#include "RtpStreamGuard.h"

enum class NtpSyncPhase : uint8_t {
    IDLE,
    SYNCING,
    SYNCED,
    FAILED,
};

class NtpSyncStateMachine {
public:
    NtpSyncStateMachine(uint32_t timeoutMs, uint32_t resyncIntervalMs,
                        uint32_t recoveryIntervalMs)
        : _timeoutMs(timeoutMs)
        , _resyncIntervalMs(resyncIntervalMs)
        , _recoveryIntervalMs(recoveryIntervalMs)
        , _backoff(5000, 60000) {}

    NtpSyncPhase phase() const { return _phase; }

    bool isSynced(uint32_t nowMs) const {
        return _phase == NtpSyncPhase::SYNCED;
    }

    bool syncInProgress() const {
        return _phase == NtpSyncPhase::SYNCING;
    }

    uint32_t lastSuccessMs() const { return _lastSuccessMs; }
    uint32_t syncGeneration() const { return _syncGeneration; }

    void requestSync(uint32_t nowMs) {
        _phase = NtpSyncPhase::SYNCING;
        _syncStartMs = nowMs;
        _syncGeneration++;
    }

    void onSntpSuccess(uint32_t nowMs, int64_t /*epochMs*/) {
        _phase = NtpSyncPhase::SYNCED;
        _lastSuccessMs = nowMs;
        _syncStartMs = nowMs;
        _backoff.recordSuccess();
    }

    void tick(uint32_t nowMs) {
        if (_phase == NtpSyncPhase::SYNCING &&
            nowMs - _syncStartMs >= _timeoutMs) {
            _phase = NtpSyncPhase::FAILED;
            _backoff.recordFailure(nowMs);
        }

        if (_phase == NtpSyncPhase::SYNCED &&
            nowMs - _lastSuccessMs >= _resyncIntervalMs) {
            requestSync(nowMs);
        } else if ((_phase == NtpSyncPhase::FAILED ||
                    _phase == NtpSyncPhase::IDLE) &&
                   _backoff.ready(nowMs) &&
                   nowMs - _lastAttemptMs >= _recoveryIntervalMs) {
            _lastAttemptMs = nowMs;
            requestSync(nowMs);
        }
    }

private:
    uint32_t _timeoutMs;
    uint32_t _resyncIntervalMs;
    uint32_t _recoveryIntervalMs;
    NtpSyncPhase _phase = NtpSyncPhase::IDLE;
    uint32_t _syncStartMs = 0;
    uint32_t _lastSuccessMs = 0;
    uint32_t _lastAttemptMs = 0;
    uint32_t _syncGeneration = 0;
    ReconnectBackoff _backoff;
};

#endif
