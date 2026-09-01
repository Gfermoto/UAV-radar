/**
 * @file    NTPClient.cpp
 * @brief   Реализация NTP-клиента.
 *
 * @author  RTSPMIC Team
 * @date    2026-07-01
 */

#include "NTPClient.h"
#include <sys/time.h>
#include <string.h>

#if !defined(UNIT_TEST)
#include <esp_sntp.h>
#endif

static const char *NTP_FALLBACK_1 = "time.google.com";
static const char *NTP_FALLBACK_2 = "time.cloudflare.com";

static NTPClient *s_ntpInstance = nullptr;

#if !defined(UNIT_TEST)
static void sntpSyncNotification(struct timeval *tv) {
    if (!s_ntpInstance || !tv) return;
    const int64_t epochMs =
        static_cast<int64_t>(tv->tv_sec) * 1000LL + tv->tv_usec / 1000;
    s_ntpInstance->notifySntpSuccess(epochMs);
}
#endif

NTPClient::NTPClient(long gmtOffsetSec, int daylightOffsetSec)
    : _gmtOffsetSec(gmtOffsetSec)
    , _daylightOffsetSec(daylightOffsetSec)
    , _state(NTP_TIMEOUT_MS, NTP_RESYNC_INTERVAL_MS, NTP_AUTO_RECOVERY_MS)
{
    strncpy(_serverPrimary, NTP_DEFAULT_SERVER, sizeof(_serverPrimary) - 1);
    _serverPrimary[sizeof(_serverPrimary) - 1] = '\0';
}

bool NTPClient::begin() {
    s_ntpInstance = this;
    configureServers();
    Serial.printf("[NTP] Запуск синхронизации (%s, таймаут %d сек)...\n",
                  _serverPrimary, NTP_TIMEOUT_MS / 1000);
    requestResync();
    return true;
}

void NTPClient::update() {
    const uint32_t now = millis();
    _state.tick(now);
}

bool NTPClient::isSynced() const {
    return _state.isSynced(millis());
}

int32_t NTPClient::getSyncAgeMs() const {
    if (!isSynced()) return -1;
    uint32_t last = _state.lastSuccessMs();
    uint32_t now = millis();
    return (int32_t)(now - last);
}

int64_t NTPClient::getEpochMillis() const {
    if (!isSynced()) return 0;

    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return (static_cast<int64_t>(tv.tv_sec) * 1000LL) + (tv.tv_usec / 1000LL);
}

time_t NTPClient::getEpoch() const {
    if (!isSynced()) return 0;
    return time(nullptr);
}

char* NTPClient::getISO8601(char *buffer, size_t bufSize) const {
    if (!isSynced() || !buffer || bufSize < 25) return nullptr;

    time_t now = time(nullptr);
    struct tm timeInfo;
    gmtime_r(&now, &timeInfo);

    snprintf(buffer, bufSize, "%04d-%02d-%02dT%02d:%02d:%02dZ",
             timeInfo.tm_year + 1900, timeInfo.tm_mon + 1, timeInfo.tm_mday,
             timeInfo.tm_hour, timeInfo.tm_min, timeInfo.tm_sec);

    return buffer;
}

bool NTPClient::forceSync() {
    requestResync();
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
    configureServers();
    _state.requestSync(millis());
#if !defined(UNIT_TEST)
    sntp_restart();
#endif
    Serial.printf("[NTP] Ресинхронизация запрошена → %s\n", _serverPrimary);
}

void NTPClient::notifySntpSuccess(int64_t epochMs) {
    if (epochMs <= static_cast<int64_t>(NTP_MIN_VALID_EPOCH) * 1000LL) return;
    _state.onSntpSuccess(millis(), epochMs);
    Serial.printf("[NTP] SNTP sync OK (gen=%u)\n", _state.syncGeneration());
}

NtpSyncPhase NTPClient::syncPhase() const {
    return _state.phase();
}

void NTPClient::configureServers() {
    const char *primary = _serverPrimary[0] ? _serverPrimary : NTP_DEFAULT_SERVER;
#if !defined(UNIT_TEST)
    configTime(_gmtOffsetSec, _daylightOffsetSec,
               primary, NTP_FALLBACK_1, NTP_FALLBACK_2);
    sntp_set_time_sync_notification_cb(sntpSyncNotification);
#else
    (void)primary;
#endif
}
