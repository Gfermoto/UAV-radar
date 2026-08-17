/**
 * @file    NTPClient_stub.cpp
 * @brief   Stub NTP с фиксированным epoch для native-тестов MQTT auth и telemetry.
 */

#include "NTPClient.h"
#include <ctime>
#include <cstring>

namespace RtspMicTest {
int64_t g_epochMillis = 1704067200000LL;
bool    g_ntpSynced   = true;
}

NTPClient::NTPClient(long gmtOffsetSec, int daylightOffsetSec)
    : _gmtOffsetSec(gmtOffsetSec)
    , _daylightOffsetSec(daylightOffsetSec)
    , _state(NTP_TIMEOUT_MS, NTP_RESYNC_INTERVAL_MS, NTP_AUTO_RECOVERY_MS)
{
    strncpy(_serverPrimary, NTP_DEFAULT_SERVER, sizeof(_serverPrimary) - 1);
    _serverPrimary[sizeof(_serverPrimary) - 1] = '\0';
    if (RtspMicTest::g_ntpSynced) {
        notifySntpSuccess(RtspMicTest::g_epochMillis);
    }
}

bool NTPClient::begin() {
    if (RtspMicTest::g_ntpSynced) {
        notifySntpSuccess(RtspMicTest::g_epochMillis);
    }
    return true;
}

void NTPClient::update() {
    _state.tick(millis());
}

bool NTPClient::isSynced() const {
    return _state.isSynced(millis());
}

int32_t NTPClient::getSyncAgeMs() const {
    if (!isSynced()) return -1;
    return (int32_t)(millis() - _state.lastSuccessMs());
}

int64_t NTPClient::getEpochMillis() const {
    return isSynced() ? RtspMicTest::g_epochMillis : 0;
}

time_t NTPClient::getEpoch() const {
    return static_cast<time_t>(getEpochMillis() / 1000);
}

char *NTPClient::getISO8601(char *buffer, size_t bufSize) const {
    if (!buffer || bufSize < 2) return nullptr;
    snprintf(buffer, bufSize, "2024-01-01T00:00:00Z");
    return buffer;
}

bool NTPClient::forceSync() {
    requestResync();
    if (RtspMicTest::g_ntpSynced) {
        notifySntpSuccess(RtspMicTest::g_epochMillis);
    }
    return isSynced();
}

void NTPClient::setServer(const char *host) {
    if (host && host[0]) {
        strncpy(_serverPrimary, host, sizeof(_serverPrimary) - 1);
        _serverPrimary[sizeof(_serverPrimary) - 1] = '\0';
    } else {
        strncpy(_serverPrimary, NTP_DEFAULT_SERVER, sizeof(_serverPrimary) - 1);
        _serverPrimary[sizeof(_serverPrimary) - 1] = '\0';
    }
    requestResync();
}

void NTPClient::requestResync() {
    _state.requestSync(millis());
    if (RtspMicTest::g_ntpSynced) {
        notifySntpSuccess(RtspMicTest::g_epochMillis);
    }
}

void NTPClient::notifySntpSuccess(int64_t epochMs) {
    if (epochMs <= static_cast<int64_t>(NTP_MIN_VALID_EPOCH) * 1000LL) return;
    _state.onSntpSuccess(millis(), epochMs);
}

NtpSyncPhase NTPClient::syncPhase() const {
    return _state.phase();
}

void NTPClient::configureServers() {}
