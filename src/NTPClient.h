/**
 * @file    NTPClient.h
 * @brief   NTP-клиент для синхронизации времени. Core 0.
 *
 * Неблокирующая state machine (NtpSyncStateMachine): synced только после
 * SNTP callback. getEpochMillis/getISO8601 — для MQTT/telemetry после sync.
 *
 * @see NtpSyncState.h, docs/ARCHITECTURE.md
 *
 * @author  RTSPMIC Team
 * @date    2026-07-01
 */

#ifndef NTP_CLIENT_H
#define NTP_CLIENT_H

#include <Arduino.h>
#include "Config.h"
#include "NtpSyncState.h"

class NTPClient {
public:
    explicit NTPClient(long gmtOffsetSec = 10800, int daylightOffsetSec = 0);

    bool begin();
    void update();
    bool isSynced() const;
    int32_t getSyncAgeMs() const;
    int64_t getEpochMillis() const;
    time_t getEpoch() const;
    char* getISO8601(char *buffer, size_t bufSize) const;
    void setServer(const char *host);
    const char *getServer() const { return _serverPrimary; }
    bool forceSync();
    void requestResync();
    NtpSyncPhase syncPhase() const;
    void notifySntpSuccess(int64_t epochMs);

private:
    long     _gmtOffsetSec;
    int      _daylightOffsetSec;
    char     _serverPrimary[64];
    NtpSyncStateMachine _state;

    void configureServers();
};

#endif // NTP_CLIENT_H
